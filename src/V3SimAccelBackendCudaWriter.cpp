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

std::vector<size_t> toSortedVector(const std::unordered_set<size_t>& in) {
    std::vector<size_t> out{in.begin(), in.end()};
    std::sort(out.begin(), out.end());
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

void emitPartitionForwardDecls(std::ofstream& of, size_t partitionCount) {
    for (size_t i = 0; i < partitionCount; ++i) {
        of << "extern \"C\" __global__ void sim_accel_eval_assignw_u32_part" << i
           << "(const uint32_t* state_in,\n"
           << "                                                        uint32_t* state_out,\n"
           << "                                                        uint32_t nstates);\n";
    }
    if (partitionCount) of << "\n";
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

void emitApiHeader(const string& filename, const std::vector<AssignPartition>& partitions) {
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
       << "extern \"C\" cudaError_t sim_accel_eval_assignw_launch_partition(uint32_t index,\n"
       << "                                                                 const uint32_t* state_in,\n"
       << "                                                                 uint32_t* state_out,\n"
       << "                                                                 uint32_t nstates,\n"
       << "                                                                 uint32_t block_size);\n"
       << "extern \"C\" cudaError_t sim_accel_eval_assignw_launch_all(const uint32_t* state_in,\n"
       << "                                                           uint32_t* state_out,\n"
       << "                                                           uint32_t nstates,\n"
       << "                                                           uint32_t block_size);\n"
       << "extern \"C\" uint32_t sim_accel_eval_partition_count();\n"
       << "extern \"C\" uint32_t sim_accel_eval_partition_assign_count(uint32_t index);\n"
       << "extern \"C\" uint32_t sim_accel_eval_partition_read_count(uint32_t index);\n"
       << "extern \"C\" uint32_t sim_accel_eval_partition_read_var_index(uint32_t index,\n"
       << "                                                              uint32_t slot);\n"
       << "extern \"C\" uint32_t sim_accel_eval_partition_write_count(uint32_t index);\n"
       << "extern \"C\" uint32_t sim_accel_eval_partition_write_var_index(uint32_t index,\n"
       << "                                                               uint32_t slot);\n"
       << "extern \"C\" uint32_t sim_accel_eval_var_count();\n"
       << "extern \"C\" const char* sim_accel_eval_var_name(uint32_t index);\n"
       << "extern \"C\" uint32_t sim_accel_eval_input_count();\n"
       << "extern \"C\" uint32_t sim_accel_eval_input_var_index(uint32_t slot);\n"
       << "extern \"C\" uint32_t sim_accel_eval_output_count();\n"
       << "extern \"C\" uint32_t sim_accel_eval_output_var_index(uint32_t slot);\n";
    emitPartitionForwardDecls(of, partitions.size());
}

void emitPartitionedAuxFiles(const string& filename, const V3SimAccelProgram& program,
                             const std::vector<EmittedAssign>& emittedAssigns,
                             const std::unordered_set<size_t>& readVarIdxs,
                             const std::unordered_set<size_t>& writtenVarIdxs,
                             const std::vector<AssignPartition>& partitions) {
    emitApiHeader(filename + ".api.h", partitions);

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
    }

    {
        std::ofstream link{filename + ".link.cu"};
        if (!link.is_open()) v3fatal("Cannot open output file: " + filename + ".link.cu");
        link << "// Generated by Verilator --sim-accel-only\n"
             << "#include <cuda_runtime.h>\n"
             << "#include <stdint.h>\n\n";
        emitPartitionForwardDecls(link, partitions.size());
        emitPartitionLaunchHelpers(link, program, partitions);
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
}

}  // namespace

size_t V3SimAccelBackendCudaWriter::write(const string& filename,
                                          const V3SimAccelProgram& program,
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
    std::unordered_set<size_t> writtenVarIdxs;
    std::unordered_set<size_t> readVarIdxs;

    for (const V3SimAccelProgram::Assign& assign : program.m_assigns) {
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
        emittedAssigns.push_back(std::move(rec));
    }

    const std::vector<AssignPartition> partitions = buildPartitions(program, emittedAssigns,
                                                                    assignsPerKernel);
    emitCpuReference(of, program, emittedAssigns, readVarIdxs, writtenVarIdxs);
    emitPartitionCpuHelpers(of, program, partitions);
    for (size_t partitionIdx = 0; partitionIdx < partitions.size(); ++partitionIdx) {
        emitPartitionKernel(of, program, partitions.at(partitionIdx), partitionIdx);
    }
    emitPartitionLaunchHelpers(of, program, partitions);
    emitProgramMetadataHelpers(of, program);
    of.close();

    emitPartitionedAuxFiles(filename, program, emittedAssigns, readVarIdxs, writtenVarIdxs,
                            partitions);

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

    return emittedAssigns.size();
}
