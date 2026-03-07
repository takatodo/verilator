// DESCRIPTION: Verilator: Sim-Accel CUDA File Writer
//
// Code available from: https://verilator.org
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2026-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//=============================================================================

#include "V3SimAccelBackendCudaWriter.h"

#include "V3Error.h"
#include "V3SimAccelBackendCudaExprEmitter.h"
#include "V3String.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <fstream>
#include <numeric>
#include <unordered_map>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace {

uint32_t simAccelMaskValue(int width) {
    if (width <= 0) return 0U;
    if (width >= 32) return 0xffffffffU;
    return static_cast<uint32_t>((1ULL << width) - 1ULL);
}

string simAccelMaskLiteral(int width) {
    std::ostringstream os;
    os << "0x" << std::hex << std::nouppercase << simAccelMaskValue(width) << "u";
    return os.str();
}

string simAccelEscapeCString(const string& in) {
    string out;
    out.reserve(in.size() + 8);
    for (char ch : in) {
        if (ch == '\\' || ch == '"') out += '\\';
        out += ch;
    }
    return out;
}

struct EmittedAssign final {
    size_t m_lhsIdx = 0;
    string m_lhsName;
    string m_expr;
    std::vector<size_t> m_rhsIdxs;
};

struct AssignPartition final {
    std::vector<EmittedAssign> m_assigns;
    std::vector<size_t> m_readVarIdxs;
    std::vector<size_t> m_writtenVarIdxs;
    std::unordered_set<size_t> m_writtenBeforeIdxs;
    string m_dominantHierarchy;
    string m_dominantHierarchyKey;
    size_t m_dominantHierarchyAssignCount = 0;
    size_t m_uniqueHierarchyCount = 0;
    string m_canonicalHash;
    size_t m_canonicalVarCount = 0;
};

struct AssignCluster final {
    size_t m_clusterIdx = 0;
    size_t m_topoRank = 0;
    uint64_t m_inputSignatureBitCount = 0;
    string m_ownerHint;
    std::vector<size_t> m_exprKindCounts;
    size_t m_operatorCount = 0;
    std::vector<EmittedAssign> m_assigns;
    std::vector<size_t> m_readVarIdxs;
    std::vector<size_t> m_writtenVarIdxs;
    std::unordered_set<size_t> m_writtenBeforeIdxs;
    string m_dominantHierarchy;
    size_t m_uniqueHierarchyCount = 0;
};

std::vector<size_t> toSortedVector(const std::unordered_set<size_t>& in) {
    std::vector<size_t> out{in.begin(), in.end()};
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<size_t> filterReadFromOutputVarIdxs(
    const std::vector<size_t>& readVarIdxs, const std::unordered_set<size_t>& writtenBeforeIdxs) {
    std::vector<size_t> out;
    out.reserve(readVarIdxs.size());
    for (const size_t readIdx : readVarIdxs) {
        if (writtenBeforeIdxs.find(readIdx) != writtenBeforeIdxs.end()) out.push_back(readIdx);
    }
    return out;
}

string simAccelPartitionHierarchyLabel(const V3SimAccelProgram::Var& var) {
    return var.m_hierarchy.empty() ? string{"<top>"} : var.m_hierarchy;
}

string simAccelSanitizeHierarchyKey(const string& label) {
    string out;
    out.reserve(label.size());
    for (char ch : label) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        } else {
            out += '_';
        }
    }
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) return "top";
    return out;
}

string simAccelCanonicalizeExpr(
    const string& expr, const std::function<string(size_t)>& canonicalVarToken) {
    string out;
    out.reserve(expr.size() + 16);
    for (size_t i = 0; i < expr.size();) {
        if (expr.compare(i, 2, "v_") == 0) {
            size_t j = i + 2;
            while (j < expr.size() && std::isdigit(static_cast<unsigned char>(expr[j]))) ++j;
            if (j > i + 2) {
                const size_t absIdx = static_cast<size_t>(std::stoul(expr.substr(i + 2, j - (i + 2))));
                out += canonicalVarToken(absIdx);
                i = j;
                continue;
            }
        }
        out += expr[i++];
    }
    return out;
}

const char* simAccelExprKindName(V3SimAccelProgram::ExprKind kind) {
    switch (kind) {
    case V3SimAccelProgram::ExprKind::VAR: return "var";
    case V3SimAccelProgram::ExprKind::CONST: return "const";
    case V3SimAccelProgram::ExprKind::AND: return "and";
    case V3SimAccelProgram::ExprKind::OR: return "or";
    case V3SimAccelProgram::ExprKind::XOR: return "xor";
    case V3SimAccelProgram::ExprKind::LOGAND: return "logand";
    case V3SimAccelProgram::ExprKind::LOGOR: return "logor";
    case V3SimAccelProgram::ExprKind::LOGNOT: return "lognot";
    case V3SimAccelProgram::ExprKind::ADD: return "add";
    case V3SimAccelProgram::ExprKind::SUB: return "sub";
    case V3SimAccelProgram::ExprKind::EQ: return "eq";
    case V3SimAccelProgram::ExprKind::NEQ: return "neq";
    case V3SimAccelProgram::ExprKind::COND: return "cond";
    case V3SimAccelProgram::ExprKind::CONCAT: return "concat";
    case V3SimAccelProgram::ExprKind::SEL: return "sel";
    case V3SimAccelProgram::ExprKind::SHIFTL: return "shiftl";
    case V3SimAccelProgram::ExprKind::SHIFTLOVR: return "shiftlovr";
    case V3SimAccelProgram::ExprKind::SHIFTR: return "shiftr";
    case V3SimAccelProgram::ExprKind::SHIFTROVR: return "shiftrovr";
    case V3SimAccelProgram::ExprKind::NOT: return "not";
    case V3SimAccelProgram::ExprKind::CCAST: return "ccast";
    case V3SimAccelProgram::ExprKind::EXTEND: return "extend";
    case V3SimAccelProgram::ExprKind::EXTENDS: return "extends";
    }
    return "unknown";
}

string simAccelExprKindCountsSummary(const std::vector<size_t>& counts) {
    std::ostringstream os;
    bool first = true;
    for (size_t kindIdx = 0; kindIdx < counts.size(); ++kindIdx) {
        const size_t count = counts.at(kindIdx);
        if (!count) continue;
        if (!first) os << ';';
        first = false;
        os << simAccelExprKindName(static_cast<V3SimAccelProgram::ExprKind>(kindIdx)) << '='
           << count;
    }
    if (first) return "-";
    return os.str();
}

void finalizePartitionCanonicalInfo(const V3SimAccelProgram& program, AssignPartition& partition) {
    std::unordered_map<size_t, size_t> canonicalVarNums;
    auto canonicalVarNumber = [&](size_t absIdx) {
        const auto it = canonicalVarNums.find(absIdx);
        if (it != canonicalVarNums.end()) return it->second;
        const size_t next = canonicalVarNums.size();
        canonicalVarNums.emplace(absIdx, next);
        return next;
    };
    auto canonicalVarToken = [&](size_t absIdx) {
        return "v_" + cvtToStr(canonicalVarNumber(absIdx));
    };

    std::ostringstream canonical;
    canonical << "assigns=" << partition.m_assigns.size() << '\n';
    for (const EmittedAssign& assign : partition.m_assigns) {
        const V3SimAccelProgram::Var& lhsVar = program.m_vars.at(assign.m_lhsIdx);
        canonical << "lhs=" << canonicalVarToken(assign.m_lhsIdx) << ":w" << lhsVar.m_width
                  << ";expr=" << simAccelCanonicalizeExpr(assign.m_expr, canonicalVarToken)
                  << ";rhs=";
        bool first = true;
        for (const size_t rhsIdx : assign.m_rhsIdxs) {
            if (!first) canonical << ',';
            first = false;
            canonical << canonicalVarToken(rhsIdx) << ":w" << program.m_vars.at(rhsIdx).m_width;
        }
        canonical << '\n';
    }
    canonical << "reads=";
    for (const size_t readIdx : partition.m_readVarIdxs) {
        canonical << canonicalVarToken(readIdx) << ":w" << program.m_vars.at(readIdx).m_width
                  << ':' << (partition.m_writtenBeforeIdxs.find(readIdx) != partition.m_writtenBeforeIdxs.end()
                                 ? "out"
                                 : "in")
                  << ';';
    }
    canonical << "\nwrites=";
    for (const size_t writeIdx : partition.m_writtenVarIdxs) {
        canonical << canonicalVarToken(writeIdx) << ":w" << program.m_vars.at(writeIdx).m_width
                  << ';';
    }
    VHashSha256 hash{canonical.str()};
    partition.m_canonicalHash = hash.digestSymbol();
    partition.m_canonicalVarCount = canonicalVarNums.size();
}

std::vector<AssignPartition> buildPartitions(const V3SimAccelProgram& program,
                                             const std::vector<EmittedAssign>& emittedAssigns,
                                             size_t assignsPerKernel) {
    const size_t chunkSize = assignsPerKernel ? assignsPerKernel : emittedAssigns.size();
    std::vector<AssignPartition> partitions;
    std::unordered_set<size_t> writtenBeforeIdxs;

    for (size_t start = 0; start < emittedAssigns.size(); start += chunkSize) {
        AssignPartition partition;
        partition.m_writtenBeforeIdxs = writtenBeforeIdxs;

        const size_t end = std::min(start + chunkSize, emittedAssigns.size());
        std::unordered_set<size_t> readVarIdxs;
        std::unordered_set<size_t> writtenVarIdxs;
        std::unordered_map<string, size_t> hierarchyCounts;
        for (size_t idx = start; idx < end; ++idx) {
            const EmittedAssign& assign = emittedAssigns.at(idx);
            partition.m_assigns.push_back(assign);
            writtenVarIdxs.emplace(assign.m_lhsIdx);
            for (const size_t rhsIdx : assign.m_rhsIdxs) readVarIdxs.emplace(rhsIdx);
            const string hierarchy
                = simAccelPartitionHierarchyLabel(program.m_vars.at(assign.m_lhsIdx));
            const size_t count = ++hierarchyCounts[hierarchy];
            if (count > partition.m_dominantHierarchyAssignCount
                || (count == partition.m_dominantHierarchyAssignCount
                    && (partition.m_dominantHierarchy.empty()
                        || hierarchy < partition.m_dominantHierarchy))) {
                partition.m_dominantHierarchy = hierarchy;
                partition.m_dominantHierarchyAssignCount = count;
            }
        }
        partition.m_readVarIdxs = toSortedVector(readVarIdxs);
        partition.m_writtenVarIdxs = toSortedVector(writtenVarIdxs);
        partition.m_uniqueHierarchyCount = hierarchyCounts.size();
        partition.m_dominantHierarchyKey
            = simAccelSanitizeHierarchyKey(partition.m_dominantHierarchy);
        finalizePartitionCanonicalInfo(program, partition);
        for (const size_t writtenIdx : partition.m_writtenVarIdxs) writtenBeforeIdxs.emplace(writtenIdx);
        partitions.push_back(std::move(partition));
    }
    return partitions;
}

std::vector<AssignCluster> buildClusters(
    const V3SimAccelProgram& program,
    const V3SimAccelProgramAnalysis::ApproxRegCutAnalysis& approxRegCut,
    const std::vector<EmittedAssign>& emittedAssigns,
    const std::vector<size_t>& programAssignToEmitted) {
    std::vector<AssignCluster> clusters(approxRegCut.m_clusters.size());
    for (const V3SimAccelProgramAnalysis::ApproxRegCutCluster& analysisCluster :
         approxRegCut.m_clusters) {
        AssignCluster& cluster = clusters.at(analysisCluster.m_clusterIdx);
        cluster.m_clusterIdx = analysisCluster.m_clusterIdx;
        cluster.m_topoRank = analysisCluster.m_topoRank;
        cluster.m_inputSignatureBitCount = analysisCluster.m_inputSignatureBitCount;
        cluster.m_ownerHint = analysisCluster.m_hybridOwnerHint;
        cluster.m_exprKindCounts = analysisCluster.m_exprKindCounts;
        cluster.m_operatorCount = analysisCluster.m_operatorCount;

        std::unordered_set<size_t> readVarIdxs;
        std::unordered_set<size_t> writtenVarIdxs;
        std::unordered_map<string, size_t> hierarchyCounts;
        size_t dominantCount = 0;
        for (const size_t assignIdx : analysisCluster.m_assignIdxs) {
            if (assignIdx >= programAssignToEmitted.size()) continue;
            const size_t emittedIdx = programAssignToEmitted.at(assignIdx);
            if (emittedIdx == static_cast<size_t>(-1) || emittedIdx >= emittedAssigns.size()) continue;
            const EmittedAssign& assign = emittedAssigns.at(emittedIdx);
            cluster.m_assigns.push_back(assign);
            writtenVarIdxs.emplace(assign.m_lhsIdx);
            for (const size_t rhsIdx : assign.m_rhsIdxs) readVarIdxs.emplace(rhsIdx);
            const string hierarchy
                = simAccelPartitionHierarchyLabel(program.m_vars.at(assign.m_lhsIdx));
            const size_t count = ++hierarchyCounts[hierarchy];
            if (count > dominantCount
                || (count == dominantCount
                    && (cluster.m_dominantHierarchy.empty()
                        || hierarchy < cluster.m_dominantHierarchy))) {
                cluster.m_dominantHierarchy = hierarchy;
                dominantCount = count;
            }
        }
        cluster.m_readVarIdxs = toSortedVector(readVarIdxs);
        cluster.m_writtenVarIdxs = toSortedVector(writtenVarIdxs);
        cluster.m_uniqueHierarchyCount = hierarchyCounts.size();
        if (cluster.m_dominantHierarchy.empty() && !cluster.m_assigns.empty()) {
            cluster.m_dominantHierarchy
                = simAccelPartitionHierarchyLabel(program.m_vars.at(cluster.m_assigns.front().m_lhsIdx));
        }
    }

    std::unordered_set<size_t> writtenBeforeIdxs;
    std::vector<size_t> topoOrder = approxRegCut.m_clusterTopoOrder;
    if (topoOrder.size() != clusters.size()) {
        topoOrder.resize(clusters.size());
        std::iota(topoOrder.begin(), topoOrder.end(), 0);
    }
    for (size_t topoRank = 0; topoRank < topoOrder.size(); ++topoRank) {
        const size_t clusterIdx = topoOrder.at(topoRank);
        if (clusterIdx >= clusters.size()) continue;
        AssignCluster& cluster = clusters.at(clusterIdx);
        cluster.m_topoRank = topoRank;
        cluster.m_writtenBeforeIdxs = writtenBeforeIdxs;
        for (const size_t writtenIdx : cluster.m_writtenVarIdxs) {
            writtenBeforeIdxs.emplace(writtenIdx);
        }
    }
    return clusters;
}

void emitVarLoads(std::ofstream& of, const V3SimAccelProgram& program,
                  const std::vector<size_t>& readVarIdxs,
                  const std::vector<size_t>& writtenVarIdxs,
                  const std::unordered_set<size_t>* readFromOutputp,
                  const string& stateInName, const string& stateOutName) {
    std::unordered_set<size_t> readVarSet{readVarIdxs.begin(), readVarIdxs.end()};
    for (const size_t varIdx : readVarIdxs) {
        const V3SimAccelProgram::Var& var = program.m_vars.at(varIdx);
        const bool readFromOutput = readFromOutputp && readFromOutputp->find(varIdx) != readFromOutputp->end();
        const string& stateName = readFromOutput ? stateOutName : stateInName;
        of << "    uint32_t v_" << varIdx << " = " << stateName << "[" << varIdx
           << "U * nstates + tid] & " << simAccelMaskLiteral(var.m_width) << ";  // "
           << var.m_name;
        if (readFromOutput) of << " (partition input)";
        of << "\n";
    }
    for (const size_t varIdx : writtenVarIdxs) {
        if (readVarSet.find(varIdx) != readVarSet.end()) continue;
        const V3SimAccelProgram::Var& var = program.m_vars.at(varIdx);
        of << "    uint32_t v_" << varIdx << " = 0u;  // " << var.m_name << " (write-only)\n";
    }
}

void emitAssignStatements(std::ofstream& of, const std::vector<EmittedAssign>& assigns) {
    for (const EmittedAssign& assign : assigns) {
        of << "    v_" << assign.m_lhsIdx << " = " << assign.m_expr << ";  // "
           << assign.m_lhsName << "\n";
    }
}

void emitPartitionStores(std::ofstream& of, const V3SimAccelProgram& program,
                         const std::vector<size_t>& writtenVarIdxs, const string& stateOutName) {
    for (const size_t varIdx : writtenVarIdxs) {
        const V3SimAccelProgram::Var& var = program.m_vars.at(varIdx);
        of << "    " << stateOutName << "[" << varIdx << "U * nstates + tid] = v_" << varIdx
           << " & " << simAccelMaskLiteral(var.m_width) << ";\n";
    }
}

void emitCpuReference(std::ofstream& of, const V3SimAccelProgram& program,
                      const std::vector<EmittedAssign>& emittedAssigns,
                      const std::unordered_set<size_t>& readVarIdxs,
                      const std::unordered_set<size_t>& writtenVarIdxs) {
    const std::vector<size_t> sortedReadVarIdxs = toSortedVector(readVarIdxs);
    const std::vector<size_t> sortedWrittenVarIdxs = toSortedVector(writtenVarIdxs);

    of << "extern \"C\" __host__ void sim_accel_eval_assignw_cpu_ref(const uint32_t* state_in,\n"
       << "                                                    uint32_t* state_out,\n"
       << "                                                    uint32_t nstates) {\n"
       << "    for (uint32_t tid = 0; tid < nstates; ++tid) {\n"
       << "// SIM_ACCEL_CPU_BODY_BEGIN\n";
    emitVarLoads(of, program, sortedReadVarIdxs, sortedWrittenVarIdxs, nullptr, "state_in", "state_out");
    of << "\n";
    emitAssignStatements(of, emittedAssigns);
    of << "\n";
    for (size_t i = 0; i < program.m_vars.size(); ++i) {
        const V3SimAccelProgram::Var& var = program.m_vars.at(i);
        if (writtenVarIdxs.find(i) != writtenVarIdxs.end()) {
            of << "    state_out[" << i << "U * nstates + tid] = v_" << i << " & "
               << simAccelMaskLiteral(var.m_width) << ";\n";
        } else {
            of << "    state_out[" << i << "U * nstates + tid] = state_in[" << i
               << "U * nstates + tid] & " << simAccelMaskLiteral(var.m_width) << ";\n";
        }
    }
    of << "// SIM_ACCEL_CPU_BODY_END\n"
       << "    }\n"
       << "}\n\n";
}

void emitPartitionCpuHelpers(std::ofstream& of, const V3SimAccelProgram& program,
                             const std::vector<AssignPartition>& partitions) {
    for (size_t partitionIdx = 0; partitionIdx < partitions.size(); ++partitionIdx) {
        const AssignPartition& partition = partitions.at(partitionIdx);
        of << "extern \"C\" __host__ void sim_accel_eval_assignw_cpu_part" << partitionIdx
           << "(const uint32_t* state_in,\n"
           << "                                                       uint32_t* state_out,\n"
           << "                                                       uint32_t nstates) {\n"
           << "    for (uint32_t tid = 0; tid < nstates; ++tid) {\n";
        emitVarLoads(of, program, partition.m_readVarIdxs, partition.m_writtenVarIdxs,
                     &partition.m_writtenBeforeIdxs, "state_in", "state_out");
        of << "\n";
        emitAssignStatements(of, partition.m_assigns);
        of << "\n";
        emitPartitionStores(of, program, partition.m_writtenVarIdxs, "state_out");
        of << "    }\n"
           << "}\n\n";
    }

    of << "extern \"C\" __host__ void sim_accel_eval_assignw_cpu_partition(uint32_t index,\n"
       << "                                                            const uint32_t* state_in,\n"
       << "                                                            uint32_t* state_out,\n"
       << "                                                            uint32_t nstates) {\n"
       << "    switch (index) {\n";
    for (size_t i = 0; i < partitions.size(); ++i) {
        of << "    case " << i << "U:\n"
           << "        sim_accel_eval_assignw_cpu_part" << i
           << "(state_in, state_out, nstates);\n"
           << "        return;\n";
    }
    of << "    default:\n"
       << "        return;\n"
       << "    }\n"
       << "}\n\n";
}

void emitPartitionKernel(std::ofstream& of, const V3SimAccelProgram& program,
                         const AssignPartition& partition, size_t partitionIdx) {
    of << "extern \"C\" __global__ void sim_accel_eval_assignw_u32_part" << partitionIdx
       << "(const uint32_t* state_in,\n"
       << "                                                        uint32_t* state_out,\n"
       << "                                                        uint32_t nstates) {\n"
       << "    const uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;\n"
       << "    if (tid >= nstates) return;\n";
    emitVarLoads(of, program, partition.m_readVarIdxs, partition.m_writtenVarIdxs,
                 &partition.m_writtenBeforeIdxs, "state_in", "state_out");
    of << "\n";
    emitAssignStatements(of, partition.m_assigns);
    of << "\n";
    emitPartitionStores(of, program, partition.m_writtenVarIdxs, "state_out");
    of << "}\n\n";
}

void emitClusterKernel(std::ofstream& of, const V3SimAccelProgram& program,
                       const AssignCluster& cluster) {
    of << "extern \"C\" __global__ void sim_accel_eval_assignw_u32_cluster"
       << cluster.m_clusterIdx << "(const uint32_t* state_in,\n"
       << "                                                           uint32_t* state_out,\n"
       << "                                                           uint32_t nstates) {\n"
       << "    const uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;\n"
       << "    if (tid >= nstates) return;\n";
    emitVarLoads(of, program, cluster.m_readVarIdxs, cluster.m_writtenVarIdxs,
                 &cluster.m_writtenBeforeIdxs, "state_in", "state_out");
    of << "\n";
    emitAssignStatements(of, cluster.m_assigns);
    of << "\n";
    emitPartitionStores(of, program, cluster.m_writtenVarIdxs, "state_out");
    of << "}\n\n";
}

void emitClusterCpuHelpers(std::ofstream& of, const V3SimAccelProgram& program,
                           const std::vector<AssignCluster>& clusters) {
    for (const AssignCluster& cluster : clusters) {
        of << "extern \"C\" __host__ void sim_accel_eval_assignw_cpu_cluster"
           << cluster.m_clusterIdx << "(const uint32_t* state_in,\n"
           << "                                                          uint32_t* state_out,\n"
           << "                                                          uint32_t nstates) {\n"
           << "    for (uint32_t tid = 0; tid < nstates; ++tid) {\n";
        emitVarLoads(of, program, cluster.m_readVarIdxs, cluster.m_writtenVarIdxs,
                     &cluster.m_writtenBeforeIdxs, "state_in", "state_out");
        of << "\n";
        emitAssignStatements(of, cluster.m_assigns);
        of << "\n";
        emitPartitionStores(of, program, cluster.m_writtenVarIdxs, "state_out");
        of << "    }\n"
           << "}\n\n";
    }

    of << "extern \"C\" __host__ void sim_accel_eval_assignw_cpu_cluster_dispatch(uint32_t index,\n"
       << "                                                                   const uint32_t* state_in,\n"
       << "                                                                   uint32_t* state_out,\n"
       << "                                                                   uint32_t nstates) {\n"
       << "    switch (index) {\n";
    for (const AssignCluster& cluster : clusters) {
        of << "    case " << cluster.m_clusterIdx << "U:\n"
           << "        sim_accel_eval_assignw_cpu_cluster" << cluster.m_clusterIdx
           << "(state_in, state_out, nstates);\n"
           << "        return;\n";
    }
    of << "    default:\n"
       << "        return;\n"
       << "    }\n"
       << "}\n\n";
}

void emitPartitionLaunchHelpers(std::ofstream& of, const V3SimAccelProgram& program,
                                const std::vector<AssignPartition>& partitions) {
    of << "extern \"C\" __host__ uint32_t sim_accel_eval_partition_count() {\n"
       << "    return " << partitions.size() << "U;\n"
       << "}\n\n";

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_partition_assign_count(uint32_t index) {\n"
       << "    switch (index) {\n";
    for (size_t i = 0; i < partitions.size(); ++i) {
        of << "    case " << i << "U: return " << partitions.at(i).m_assigns.size() << "U;\n";
    }
    of << "    default: return 0U;\n"
       << "    }\n"
       << "}\n\n";

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_partition_read_count(uint32_t index) {\n"
       << "    switch (index) {\n";
    for (size_t i = 0; i < partitions.size(); ++i) {
        of << "    case " << i << "U: return " << partitions.at(i).m_readVarIdxs.size() << "U;\n";
    }
    of << "    default: return 0U;\n"
       << "    }\n"
       << "}\n\n";

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_partition_read_var_index(uint32_t index,\n"
       << "                                                               uint32_t slot) {\n"
       << "    switch (index) {\n";
    for (size_t i = 0; i < partitions.size(); ++i) {
        of << "    case " << i << "U:\n"
           << "        switch (slot) {\n";
        for (size_t slot = 0; slot < partitions.at(i).m_readVarIdxs.size(); ++slot) {
            of << "        case " << slot << "U: return "
               << partitions.at(i).m_readVarIdxs.at(slot) << "U;\n";
        }
        of << "        default: return 0xffffffffU;\n"
           << "        }\n";
    }
    of << "    default: return 0xffffffffU;\n"
       << "    }\n"
       << "}\n\n";

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_partition_read_from_output_count(uint32_t index) {\n"
       << "    switch (index) {\n";
    for (size_t i = 0; i < partitions.size(); ++i) {
        const std::vector<size_t> readFromOutput
            = filterReadFromOutputVarIdxs(partitions.at(i).m_readVarIdxs,
                                          partitions.at(i).m_writtenBeforeIdxs);
        of << "    case " << i << "U: return " << readFromOutput.size() << "U;\n";
    }
    of << "    default: return 0U;\n"
       << "    }\n"
       << "}\n\n";

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_partition_read_from_output_var_index(uint32_t index,\n"
       << "                                                                           uint32_t slot) {\n"
       << "    switch (index) {\n";
    for (size_t i = 0; i < partitions.size(); ++i) {
        const std::vector<size_t> readFromOutput
            = filterReadFromOutputVarIdxs(partitions.at(i).m_readVarIdxs,
                                          partitions.at(i).m_writtenBeforeIdxs);
        of << "    case " << i << "U:\n"
           << "        switch (slot) {\n";
        for (size_t slot = 0; slot < readFromOutput.size(); ++slot) {
            of << "        case " << slot << "U: return " << readFromOutput.at(slot) << "U;\n";
        }
        of << "        default: return 0xffffffffU;\n"
           << "        }\n";
    }
    of << "    default: return 0xffffffffU;\n"
       << "    }\n"
       << "}\n\n";

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_partition_write_count(uint32_t index) {\n"
       << "    switch (index) {\n";
    for (size_t i = 0; i < partitions.size(); ++i) {
        of << "    case " << i << "U: return " << partitions.at(i).m_writtenVarIdxs.size()
           << "U;\n";
    }
    of << "    default: return 0U;\n"
       << "    }\n"
       << "}\n\n";

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_partition_write_var_index(uint32_t index,\n"
       << "                                                                uint32_t slot) {\n"
       << "    switch (index) {\n";
    for (size_t i = 0; i < partitions.size(); ++i) {
        of << "    case " << i << "U:\n"
           << "        switch (slot) {\n";
        for (size_t slot = 0; slot < partitions.at(i).m_writtenVarIdxs.size(); ++slot) {
            of << "        case " << slot << "U: return "
               << partitions.at(i).m_writtenVarIdxs.at(slot) << "U;\n";
        }
        of << "        default: return 0xffffffffU;\n"
           << "        }\n";
    }
    of << "    default: return 0xffffffffU;\n"
       << "    }\n"
       << "}\n\n";

    of << "extern \"C\" __host__ cudaError_t sim_accel_eval_assignw_launch_partition(uint32_t index,\n"
       << "                                                                 const uint32_t* state_in,\n"
       << "                                                                 uint32_t* state_out,\n"
       << "                                                                 uint32_t nstates,\n"
       << "                                                                 uint32_t block_size) {\n"
       << "    const uint32_t block = block_size ? block_size : 256U;\n"
       << "    const uint32_t grid = (nstates + block - 1U) / block;\n"
       << "    switch (index) {\n";
    for (size_t i = 0; i < partitions.size(); ++i) {
        of << "    case " << i << "U:\n"
           << "        sim_accel_eval_assignw_u32_part" << i
           << "<<<grid, block>>>(state_in, state_out, nstates);\n"
           << "        return cudaGetLastError();\n";
    }
    of << "    default:\n"
       << "        return cudaErrorInvalidValue;\n"
       << "    }\n"
       << "}\n\n";

    of << "extern \"C\" __host__ cudaError_t sim_accel_eval_assignw_launch_all(const uint32_t* state_in,\n"
       << "                                                           uint32_t* state_out,\n"
       << "                                                           uint32_t nstates,\n"
       << "                                                           uint32_t block_size) {\n"
       << "    const uint32_t block = block_size ? block_size : 256U;\n"
       << "    const uint32_t grid = (nstates + block - 1U) / block;\n"
       << "    cudaError_t status = cudaMemcpy(state_out, state_in, " << program.m_vars.size()
       << "U * static_cast<size_t>(nstates) * sizeof(uint32_t), cudaMemcpyDeviceToDevice);\n"
       << "    if (status != cudaSuccess) return status;\n";
    for (size_t i = 0; i < partitions.size(); ++i) {
        of << "    sim_accel_eval_assignw_u32_part" << i
           << "<<<grid, block>>>(state_in, state_out, nstates);\n";
        of << "    status = cudaGetLastError();\n";
        of << "    if (status != cudaSuccess) return status;\n";
    }
    of << "    return cudaSuccess;\n"
       << "}\n\n";
}

void emitClusterLaunchHelpers(std::ofstream& of, const std::vector<AssignCluster>& clusters) {
    of << "extern \"C\" __host__ uint32_t sim_accel_eval_cluster_count() {\n"
       << "    return " << clusters.size() << "U;\n"
       << "}\n\n";

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_cluster_assign_count(uint32_t index) {\n"
       << "    switch (index) {\n";
    for (const AssignCluster& cluster : clusters) {
        of << "    case " << cluster.m_clusterIdx << "U: return " << cluster.m_assigns.size()
           << "U;\n";
    }
    of << "    default: return 0U;\n"
       << "    }\n"
       << "}\n\n";

    std::vector<const AssignCluster*> topoOrdered;
    topoOrdered.reserve(clusters.size());
    for (const AssignCluster& cluster : clusters) topoOrdered.push_back(&cluster);
    std::sort(topoOrdered.begin(), topoOrdered.end(),
              [](const AssignCluster* lhs, const AssignCluster* rhs) {
                  if (lhs->m_topoRank != rhs->m_topoRank) return lhs->m_topoRank < rhs->m_topoRank;
                  return lhs->m_clusterIdx < rhs->m_clusterIdx;
              });

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_cluster_topo_count() {\n"
       << "    return " << topoOrdered.size() << "U;\n"
       << "}\n\n";

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_cluster_topo_index(uint32_t slot) {\n"
       << "    switch (slot) {\n";
    for (size_t slot = 0; slot < topoOrdered.size(); ++slot) {
        of << "    case " << slot << "U: return " << topoOrdered.at(slot)->m_clusterIdx << "U;\n";
    }
    of << "    default: return 0xffffffffU;\n"
       << "    }\n"
       << "}\n\n";

    of << "extern \"C\" __host__ uint64_t sim_accel_eval_cluster_input_signature_bits(uint32_t index) {\n"
       << "    switch (index) {\n";
    for (const AssignCluster& cluster : clusters) {
        of << "    case " << cluster.m_clusterIdx << "U: return "
           << cluster.m_inputSignatureBitCount << "ULL;\n";
    }
    of << "    default: return 0ULL;\n"
       << "    }\n"
       << "}\n\n";

    of << "extern \"C\" __host__ const char* sim_accel_eval_cluster_owner_hint(uint32_t index) {\n"
       << "    switch (index) {\n";
    for (const AssignCluster& cluster : clusters) {
        of << "    case " << cluster.m_clusterIdx << "U: return \""
           << simAccelEscapeCString(cluster.m_ownerHint) << "\";\n";
    }
    of << "    default: return \"\";\n"
       << "    }\n"
       << "}\n\n";

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_cluster_read_count(uint32_t index) {\n"
       << "    switch (index) {\n";
    for (const AssignCluster& cluster : clusters) {
        of << "    case " << cluster.m_clusterIdx << "U: return " << cluster.m_readVarIdxs.size()
           << "U;\n";
    }
    of << "    default: return 0U;\n"
       << "    }\n"
       << "}\n\n";

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_cluster_read_var_index(uint32_t index,\n"
       << "                                                             uint32_t slot) {\n"
       << "    switch (index) {\n";
    for (const AssignCluster& cluster : clusters) {
        of << "    case " << cluster.m_clusterIdx << "U:\n"
           << "        switch (slot) {\n";
        for (size_t slot = 0; slot < cluster.m_readVarIdxs.size(); ++slot) {
            of << "        case " << slot << "U: return " << cluster.m_readVarIdxs.at(slot)
               << "U;\n";
        }
        of << "        default: return 0xffffffffU;\n"
           << "        }\n";
    }
    of << "    default: return 0xffffffffU;\n"
       << "    }\n"
       << "}\n\n";

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_cluster_read_from_output_count(uint32_t index) {\n"
       << "    switch (index) {\n";
    for (const AssignCluster& cluster : clusters) {
        const std::vector<size_t> readFromOutput
            = filterReadFromOutputVarIdxs(cluster.m_readVarIdxs, cluster.m_writtenBeforeIdxs);
        of << "    case " << cluster.m_clusterIdx << "U: return " << readFromOutput.size()
           << "U;\n";
    }
    of << "    default: return 0U;\n"
       << "    }\n"
       << "}\n\n";

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_cluster_read_from_output_var_index(uint32_t index,\n"
       << "                                                                         uint32_t slot) {\n"
       << "    switch (index) {\n";
    for (const AssignCluster& cluster : clusters) {
        const std::vector<size_t> readFromOutput
            = filterReadFromOutputVarIdxs(cluster.m_readVarIdxs, cluster.m_writtenBeforeIdxs);
        of << "    case " << cluster.m_clusterIdx << "U:\n"
           << "        switch (slot) {\n";
        for (size_t slot = 0; slot < readFromOutput.size(); ++slot) {
            of << "        case " << slot << "U: return " << readFromOutput.at(slot) << "U;\n";
        }
        of << "        default: return 0xffffffffU;\n"
           << "        }\n";
    }
    of << "    default: return 0xffffffffU;\n"
       << "    }\n"
       << "}\n\n";

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_cluster_write_count(uint32_t index) {\n"
       << "    switch (index) {\n";
    for (const AssignCluster& cluster : clusters) {
        of << "    case " << cluster.m_clusterIdx << "U: return "
           << cluster.m_writtenVarIdxs.size() << "U;\n";
    }
    of << "    default: return 0U;\n"
       << "    }\n"
       << "}\n\n";

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_cluster_write_var_index(uint32_t index,\n"
       << "                                                              uint32_t slot) {\n"
       << "    switch (index) {\n";
    for (const AssignCluster& cluster : clusters) {
        of << "    case " << cluster.m_clusterIdx << "U:\n"
           << "        switch (slot) {\n";
        for (size_t slot = 0; slot < cluster.m_writtenVarIdxs.size(); ++slot) {
            of << "        case " << slot << "U: return "
               << cluster.m_writtenVarIdxs.at(slot) << "U;\n";
        }
        of << "        default: return 0xffffffffU;\n"
           << "        }\n";
    }
    of << "    default: return 0xffffffffU;\n"
       << "    }\n"
       << "}\n\n";

    of << "extern \"C\" __host__ cudaError_t sim_accel_eval_assignw_launch_cluster(uint32_t index,\n"
       << "                                                               const uint32_t* state_in,\n"
       << "                                                               uint32_t* state_out,\n"
       << "                                                               uint32_t nstates,\n"
       << "                                                               uint32_t block_size) {\n"
       << "    const uint32_t block = block_size ? block_size : 256U;\n"
       << "    const uint32_t grid = (nstates + block - 1U) / block;\n"
       << "    switch (index) {\n";
    for (const AssignCluster& cluster : clusters) {
        of << "    case " << cluster.m_clusterIdx << "U:\n"
           << "        sim_accel_eval_assignw_u32_cluster" << cluster.m_clusterIdx
           << "<<<grid, block>>>(state_in, state_out, nstates);\n"
           << "        return cudaGetLastError();\n";
    }
    of << "    default:\n"
       << "        return cudaErrorInvalidValue;\n"
       << "    }\n"
       << "}\n\n";
}

void emitPartitionForwardDecls(std::ofstream& of, size_t partitionCount) {
    for (size_t i = 0; i < partitionCount; ++i) {
        of << "extern \"C\" __global__ void sim_accel_eval_assignw_u32_part" << i
           << "(const uint32_t* state_in,\n"
           << "                                                        uint32_t* state_out,\n"
           << "                                                        uint32_t nstates);\n";
    }
    if (partitionCount) of << "\n";
}

void emitClusterForwardDecls(std::ofstream& of, const std::vector<AssignCluster>& clusters) {
    for (const AssignCluster& cluster : clusters) {
        of << "extern \"C\" __global__ void sim_accel_eval_assignw_u32_cluster"
           << cluster.m_clusterIdx << "(const uint32_t* state_in,\n"
           << "                                                           uint32_t* state_out,\n"
           << "                                                           uint32_t nstates);\n";
    }
    if (!clusters.empty()) of << "\n";
}

void emitProgramMetadataHelpers(std::ofstream& of, const V3SimAccelProgram& program) {
    of << "extern \"C\" __host__ uint32_t sim_accel_eval_var_count() {\n"
       << "    return " << program.m_vars.size() << "U;\n"
       << "}\n\n";

    of << "extern \"C\" __host__ const char* sim_accel_eval_var_name(uint32_t index) {\n"
       << "    switch (index) {\n";
    for (size_t i = 0; i < program.m_vars.size(); ++i) {
        of << "    case " << i << "U: return \""
           << simAccelEscapeCString(program.m_vars.at(i).m_name) << "\";\n";
    }
    of << "    default: return \"\";\n"
       << "    }\n"
       << "}\n";

    of << "\nextern \"C\" __host__ uint32_t sim_accel_eval_input_count() {\n"
       << "    return " << program.m_commPlan.m_cpuToGpuVarIdxs.size() << "U;\n"
       << "}\n";
    of << "\nextern \"C\" __host__ uint32_t sim_accel_eval_input_var_index(uint32_t slot) {\n"
       << "    switch (slot) {\n";
    for (size_t i = 0; i < program.m_commPlan.m_cpuToGpuVarIdxs.size(); ++i) {
        of << "    case " << i << "U: return "
           << program.m_commPlan.m_cpuToGpuVarIdxs.at(i) << "U;\n";
    }
    of << "    default: return 0xffffffffU;\n"
       << "    }\n"
       << "}\n";

    of << "\nextern \"C\" __host__ uint32_t sim_accel_eval_output_count() {\n"
       << "    return " << program.m_commPlan.m_gpuToCpuVarIdxs.size() << "U;\n"
       << "}\n";
    of << "\nextern \"C\" __host__ uint32_t sim_accel_eval_output_var_index(uint32_t slot) {\n"
       << "    switch (slot) {\n";
    for (size_t i = 0; i < program.m_commPlan.m_gpuToCpuVarIdxs.size(); ++i) {
        of << "    case " << i << "U: return "
           << program.m_commPlan.m_gpuToCpuVarIdxs.at(i) << "U;\n";
    }
    of << "    default: return 0xffffffffU;\n"
       << "    }\n"
       << "}\n";
}

void emitApiHeader(const string& filename, const std::vector<AssignPartition>& partitions,
                   const std::vector<AssignCluster>& clusters) {
    std::ofstream of{filename};
    if (!of.is_open()) v3fatal("Cannot open output file: " + filename);  // LCOV_EXCL_LINE
    of << "// Generated by Verilator --sim-accel-only\n"
       << "#pragma once\n"
       << "#include <cuda_runtime.h>\n"
       << "#include <stdint.h>\n\n"
       << "extern \"C\" void sim_accel_eval_assignw_cpu_ref(const uint32_t* state_in,\n"
       << "                                                  uint32_t* state_out,\n"
       << "                                                  uint32_t nstates);\n"
       << "extern \"C\" void sim_accel_eval_assignw_cpu_partition(uint32_t index,\n"
       << "                                                        const uint32_t* state_in,\n"
       << "                                                        uint32_t* state_out,\n"
       << "                                                        uint32_t nstates);\n"
       << "extern \"C\" void sim_accel_eval_assignw_cpu_cluster_dispatch(uint32_t index,\n"
       << "                                                               const uint32_t* state_in,\n"
       << "                                                               uint32_t* state_out,\n"
       << "                                                               uint32_t nstates);\n"
       << "extern \"C\" cudaError_t sim_accel_eval_assignw_launch_partition(uint32_t index,\n"
       << "                                                                 const uint32_t* state_in,\n"
       << "                                                                 uint32_t* state_out,\n"
       << "                                                                 uint32_t nstates,\n"
       << "                                                                 uint32_t block_size);\n"
       << "extern \"C\" cudaError_t sim_accel_eval_assignw_launch_cluster(uint32_t index,\n"
       << "                                                               const uint32_t* state_in,\n"
       << "                                                               uint32_t* state_out,\n"
       << "                                                               uint32_t nstates,\n"
       << "                                                               uint32_t block_size);\n"
       << "extern \"C\" cudaError_t sim_accel_eval_assignw_launch_all(const uint32_t* state_in,\n"
       << "                                                           uint32_t* state_out,\n"
       << "                                                           uint32_t nstates,\n"
       << "                                                           uint32_t block_size);\n"
       << "extern \"C\" uint32_t sim_accel_eval_partition_count();\n"
       << "extern \"C\" uint32_t sim_accel_eval_partition_assign_count(uint32_t index);\n"
       << "extern \"C\" uint32_t sim_accel_eval_partition_read_count(uint32_t index);\n"
       << "extern \"C\" uint32_t sim_accel_eval_partition_read_var_index(uint32_t index,\n"
       << "                                                              uint32_t slot);\n"
       << "extern \"C\" uint32_t sim_accel_eval_partition_read_from_output_count(uint32_t index);\n"
       << "extern \"C\" uint32_t sim_accel_eval_partition_read_from_output_var_index(uint32_t index,\n"
       << "                                                                          uint32_t slot);\n"
       << "extern \"C\" uint32_t sim_accel_eval_partition_write_count(uint32_t index);\n"
       << "extern \"C\" uint32_t sim_accel_eval_partition_write_var_index(uint32_t index,\n"
       << "                                                               uint32_t slot);\n"
       << "extern \"C\" uint32_t sim_accel_eval_cluster_count();\n"
       << "extern \"C\" uint32_t sim_accel_eval_cluster_assign_count(uint32_t index);\n"
       << "extern \"C\" uint32_t sim_accel_eval_cluster_topo_count();\n"
       << "extern \"C\" uint32_t sim_accel_eval_cluster_topo_index(uint32_t slot);\n"
       << "extern \"C\" uint64_t sim_accel_eval_cluster_input_signature_bits(uint32_t index);\n"
       << "extern \"C\" const char* sim_accel_eval_cluster_owner_hint(uint32_t index);\n"
       << "extern \"C\" uint32_t sim_accel_eval_cluster_read_count(uint32_t index);\n"
       << "extern \"C\" uint32_t sim_accel_eval_cluster_read_var_index(uint32_t index,\n"
       << "                                                            uint32_t slot);\n"
       << "extern \"C\" uint32_t sim_accel_eval_cluster_read_from_output_count(uint32_t index);\n"
       << "extern \"C\" uint32_t sim_accel_eval_cluster_read_from_output_var_index(uint32_t index,\n"
       << "                                                                        uint32_t slot);\n"
       << "extern \"C\" uint32_t sim_accel_eval_cluster_write_count(uint32_t index);\n"
       << "extern \"C\" uint32_t sim_accel_eval_cluster_write_var_index(uint32_t index,\n"
       << "                                                             uint32_t slot);\n"
       << "extern \"C\" uint32_t sim_accel_eval_var_count();\n"
       << "extern \"C\" const char* sim_accel_eval_var_name(uint32_t index);\n"
       << "extern \"C\" uint32_t sim_accel_eval_input_count();\n"
       << "extern \"C\" uint32_t sim_accel_eval_input_var_index(uint32_t slot);\n"
       << "extern \"C\" uint32_t sim_accel_eval_output_count();\n"
       << "extern \"C\" uint32_t sim_accel_eval_output_var_index(uint32_t slot);\n";
    emitPartitionForwardDecls(of, partitions.size());
    emitClusterForwardDecls(of, clusters);
}

void emitPartitionedAuxFiles(const string& filename, const V3SimAccelProgram& program,
                             const std::vector<EmittedAssign>& emittedAssigns,
                             const std::unordered_set<size_t>& readVarIdxs,
                             const std::unordered_set<size_t>& writtenVarIdxs,
                             const std::vector<AssignPartition>& partitions,
                             const std::vector<AssignCluster>& clusters) {
    emitApiHeader(filename + ".api.h", partitions, clusters);

    {
        std::ofstream cpu{filename + ".cpu.cpp"};
        if (!cpu.is_open()) v3fatal("Cannot open output file: " + filename + ".cpu.cpp");
        cpu << "// Generated by Verilator --sim-accel-only\n"
            << "#include <stdint.h>\n"
            << "#ifndef __host__\n"
            << "# define __host__\n"
            << "#endif\n\n";
        emitCpuReference(cpu, program, emittedAssigns, readVarIdxs, writtenVarIdxs);
        emitPartitionCpuHelpers(cpu, program, partitions);
        emitClusterCpuHelpers(cpu, program, clusters);
    }

    {
        std::ofstream link{filename + ".link.cu"};
        if (!link.is_open()) v3fatal("Cannot open output file: " + filename + ".link.cu");
        link << "// Generated by Verilator --sim-accel-only\n"
             << "#include <cuda_runtime.h>\n"
             << "#include <stdint.h>\n\n";
        emitPartitionForwardDecls(link, partitions.size());
        emitClusterForwardDecls(link, clusters);
        emitPartitionLaunchHelpers(link, program, partitions);
        emitClusterLaunchHelpers(link, clusters);
        emitProgramMetadataHelpers(link, program);
    }

    {
        std::ofstream partMeta{filename + ".partitions.tsv"};
        if (partMeta.is_open()) {
            partMeta << "index\tassign_count\tread_var_count\twritten_var_count"
                        "\tdominant_hierarchy\tdominant_hierarchy_key"
                        "\tdominant_hierarchy_assign_count\tunique_hierarchy_count"
                        "\tcanonical_hash\tcanonical_var_count\n";
            for (size_t i = 0; i < partitions.size(); ++i) {
                const AssignPartition& partition = partitions.at(i);
                partMeta << i << '\t' << partition.m_assigns.size() << '\t'
                         << partition.m_readVarIdxs.size() << '\t'
                         << partition.m_writtenVarIdxs.size() << '\t'
                         << partition.m_dominantHierarchy << '\t'
                         << partition.m_dominantHierarchyKey << '\t'
                         << partition.m_dominantHierarchyAssignCount << '\t'
                         << partition.m_uniqueHierarchyCount << '\t'
                         << partition.m_canonicalHash << '\t'
                         << partition.m_canonicalVarCount << '\n';
            }
        }
    }

    {
        std::ofstream clusterMeta{filename + ".clusters.tsv"};
        if (clusterMeta.is_open()) {
            clusterMeta << "index\ttopo_rank\tassign_count\tread_var_count\twritten_var_count"
                           "\tinput_signature_bits\towner_hint\tdominant_hierarchy"
                           "\tunique_hierarchy_count\toperator_count\texpr_kind_counts\n";
            for (const AssignCluster& cluster : clusters) {
                clusterMeta << cluster.m_clusterIdx << '\t' << cluster.m_topoRank << '\t'
                            << cluster.m_assigns.size() << '\t' << cluster.m_readVarIdxs.size()
                            << '\t' << cluster.m_writtenVarIdxs.size() << '\t'
                            << cluster.m_inputSignatureBitCount << '\t' << cluster.m_ownerHint
                            << '\t' << cluster.m_dominantHierarchy << '\t'
                            << cluster.m_uniqueHierarchyCount << '\t'
                            << cluster.m_operatorCount << '\t'
                            << simAccelExprKindCountsSummary(cluster.m_exprKindCounts) << '\n';
            }
        }
    }

    for (size_t partitionIdx = 0; partitionIdx < partitions.size(); ++partitionIdx) {
        std::ofstream part{filename + ".part" + cvtToStr(partitionIdx) + ".cu"};
        if (!part.is_open()) {
            v3fatal("Cannot open output file: "
                    + filename + ".part" + cvtToStr(partitionIdx) + ".cu");
        }
        part << "// Generated by Verilator --sim-accel-only\n"
             << "#include <cuda_runtime.h>\n"
             << "#include <stdint.h>\n\n";
        emitPartitionKernel(part, program, partitions.at(partitionIdx), partitionIdx);
    }

    for (const AssignCluster& cluster : clusters) {
        std::ofstream clusterOf{filename + ".cluster" + cvtToStr(cluster.m_clusterIdx) + ".cu"};
        if (!clusterOf.is_open()) {
            v3fatal("Cannot open output file: "
                    + filename + ".cluster" + cvtToStr(cluster.m_clusterIdx) + ".cu");
        }
        clusterOf << "// Generated by Verilator --sim-accel-only\n"
                  << "#include <cuda_runtime.h>\n"
                  << "#include <stdint.h>\n\n";
        emitClusterKernel(clusterOf, program, cluster);
    }
}

}  // namespace

size_t V3SimAccelBackendCudaWriter::write(
    const string& filename, const V3SimAccelProgram& program,
    const V3SimAccelProgramAnalysis::ApproxRegCutAnalysis& approxRegCut,
    size_t assignsPerKernel) {
    std::ofstream of{filename};
    if (!of.is_open()) v3fatal("Cannot open output file: " + filename);  // LCOV_EXCL_LINE

    of << "// Generated by Verilator --sim-accel-only\n"
       << "// Supported node subset: ASSIGNW + bool/arith/select/shift/concat (+ casts/extends)\n"
       << "#include <cuda_runtime.h>\n"
       << "#include <stdint.h>\n\n";

    const V3SimAccelBackendCudaExprEmitter emitter{program};
    std::unordered_set<string> emittedAssignKeys;
    std::vector<EmittedAssign> emittedAssigns;
    std::vector<size_t> programAssignToEmitted(program.m_assigns.size(), static_cast<size_t>(-1));
    std::unordered_set<size_t> writtenVarIdxs;
    std::unordered_set<size_t> readVarIdxs;

    for (size_t assignIdx = 0; assignIdx < program.m_assigns.size(); ++assignIdx) {
        const V3SimAccelProgram::Assign& assign = program.m_assigns.at(assignIdx);
        const string expr = emitter.emit(assign.m_exprIdx);
        if (expr == ("v_" + cvtToStr(assign.m_lhsIdx))) continue;
        const string key = cvtToStr(assign.m_lhsIdx) + "|" + expr;
        if (!emittedAssignKeys.emplace(key).second) continue;

        EmittedAssign rec;
        rec.m_lhsIdx = assign.m_lhsIdx;
        rec.m_lhsName = assign.m_lhsName;
        rec.m_expr = expr;
        rec.m_rhsIdxs = assign.m_rhsIdxs;
        for (const size_t rhsIdx : rec.m_rhsIdxs) readVarIdxs.emplace(rhsIdx);
        writtenVarIdxs.emplace(rec.m_lhsIdx);
        programAssignToEmitted.at(assignIdx) = emittedAssigns.size();
        emittedAssigns.push_back(std::move(rec));
    }

    const std::vector<AssignPartition> partitions = buildPartitions(program, emittedAssigns,
                                                                    assignsPerKernel);
    const std::vector<AssignCluster> clusters
        = buildClusters(program, approxRegCut, emittedAssigns, programAssignToEmitted);
    emitCpuReference(of, program, emittedAssigns, readVarIdxs, writtenVarIdxs);
    emitPartitionCpuHelpers(of, program, partitions);
    emitClusterCpuHelpers(of, program, clusters);
    for (size_t partitionIdx = 0; partitionIdx < partitions.size(); ++partitionIdx) {
        emitPartitionKernel(of, program, partitions.at(partitionIdx), partitionIdx);
    }
    for (const AssignCluster& cluster : clusters) emitClusterKernel(of, program, cluster);
    emitPartitionLaunchHelpers(of, program, partitions);
    emitClusterLaunchHelpers(of, clusters);
    emitProgramMetadataHelpers(of, program);
    of.close();

    emitPartitionedAuxFiles(filename, program, emittedAssigns, readVarIdxs, writtenVarIdxs,
                            partitions, clusters);

    const string metaFilename = filename + ".vars.tsv";
    std::ofstream met{metaFilename};
    if (met.is_open()) {
        met << "index\tname\thierarchy\tdirection\tis_primary_io\twidth\tis_activator"
               "\tis_cpu_visible\tis_gpu_input\tis_gpu_output\tinput_slot\toutput_slot\n";
        for (size_t i = 0; i < program.m_vars.size(); ++i) {
            const V3SimAccelProgram::Var& var = program.m_vars.at(i);
            met << i << '\t' << var.m_name << '\t' << var.m_hierarchy << '\t'
                << var.m_direction << '\t' << (var.m_isPrimaryIo ? "1" : "0") << '\t'
                << var.m_width << '\t' << (var.m_isActivator ? "1" : "0") << '\t'
                << (var.m_isCpuVisible ? "1" : "0") << '\t' << (var.m_isGpuInput ? "1" : "0")
                << '\t' << (var.m_isGpuOutput ? "1" : "0") << '\t';
            if (var.m_inputSlot == V3SimAccelProgram::INVALID_SLOT) {
                met << '-';
            } else {
                met << var.m_inputSlot;
            }
            met << '\t';
            if (var.m_outputSlot == V3SimAccelProgram::INVALID_SLOT) {
                met << '-';
            } else {
                met << var.m_outputSlot;
            }
            met << '\n';
        }
    }

    const string depsFilename = filename + ".deps.tsv";
    std::ofstream dep{depsFilename};
    if (dep.is_open()) {
        dep << "lhs_idx\trhs_idx_list\n";
        for (const EmittedAssign& assign : emittedAssigns) {
            dep << assign.m_lhsIdx << "\t";
            bool first = true;
            for (const size_t rhsIdx : assign.m_rhsIdxs) {
                if (!first) dep << ",";
                dep << rhsIdx;
                first = false;
            }
            dep << "\n";
        }
    }

    const string commFilename = filename + ".comm.tsv";
    std::ofstream comm{commFilename};
    if (comm.is_open()) {
        comm << "direction\tslot\tvar_idx\tname\twidth\tis_cpu_visible\n";
        for (size_t slot = 0; slot < program.m_commPlan.m_cpuToGpuVarIdxs.size(); ++slot) {
            const size_t varIdx = program.m_commPlan.m_cpuToGpuVarIdxs.at(slot);
            const V3SimAccelProgram::Var& var = program.m_vars.at(varIdx);
            comm << "cpu_to_gpu\t" << slot << '\t' << varIdx << '\t' << var.m_name << '\t'
                 << var.m_width << '\t' << (var.m_isCpuVisible ? "1" : "0") << '\n';
        }
        for (size_t slot = 0; slot < program.m_commPlan.m_gpuToCpuVarIdxs.size(); ++slot) {
            const size_t varIdx = program.m_commPlan.m_gpuToCpuVarIdxs.at(slot);
            const V3SimAccelProgram::Var& var = program.m_vars.at(varIdx);
            comm << "gpu_to_cpu\t" << slot << '\t' << varIdx << '\t' << var.m_name << '\t'
                 << var.m_width << '\t' << (var.m_isCpuVisible ? "1" : "0") << '\n';
        }
    }

    const string preloadTargetsFilename = filename + ".preload_targets.tsv";
    std::ofstream preload{preloadTargetsFilename};
    if (preload.is_open()) {
        preload << "kind\tname\ttarget_path\tast_name\thierarchy\tword_bits\tdepth\tbase_addr\t"
                   "address_unit_bytes\tendianness\tis_primary_io\n";
        for (const V3SimAccelProgram::PreloadTarget& target : program.m_preloadTargets) {
            preload << target.m_kind << '\t' << target.m_name << '\t' << target.m_targetPath
                    << '\t' << target.m_astName << '\t' << target.m_hierarchy << '\t'
                    << target.m_wordBits << '\t' << target.m_depth << '\t'
                    << target.m_baseAddr << '\t' << target.m_addressUnitBytes << '\t'
                    << target.m_endianness << '\t'
                    << (target.m_isPrimaryIo ? "1" : "0") << '\n';
        }
    }

    return emittedAssigns.size();
}
