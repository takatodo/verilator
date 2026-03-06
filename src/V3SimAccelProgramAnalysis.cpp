// DESCRIPTION: Verilator: Sim-Accel Program Analysis Helpers
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

#include "V3SimAccelProgramAnalysis.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

class DisjointSets final {
private:
    std::vector<size_t> m_parent;
    std::vector<size_t> m_rank;

public:
    explicit DisjointSets(size_t count)
        : m_parent(count)
        , m_rank(count, 0) {
        for (size_t i = 0; i < count; ++i) m_parent[i] = i;
    }

    size_t find(size_t idx) {
        if (m_parent.at(idx) == idx) return idx;
        m_parent.at(idx) = find(m_parent.at(idx));
        return m_parent.at(idx);
    }

    void unite(size_t lhs, size_t rhs) {
        size_t a = find(lhs);
        size_t b = find(rhs);
        if (a == b) return;
        if (m_rank.at(a) < m_rank.at(b)) std::swap(a, b);
        m_parent.at(b) = a;
        if (m_rank.at(a) == m_rank.at(b)) ++m_rank.at(a);
    }
};

struct TempCluster final {
    std::unordered_set<size_t> m_readVars;
    std::unordered_set<size_t> m_writtenVars;
    std::unordered_map<string, size_t> m_hierarchyCounts;
    size_t m_assignCount = 0;
};

uint64_t sumVarWidths(const V3SimAccelProgram& program, const std::unordered_set<size_t>& varIdxs) {
    uint64_t total = 0;
    for (const size_t varIdx : varIdxs) total += program.m_vars.at(varIdx).m_width;
    return total;
}

}  // namespace

V3SimAccelProgramAnalysis::ApproxRegCutSummary
V3SimAccelProgramAnalysis::analyzeApproxRegCut(const V3SimAccelProgram& program) {
    ApproxRegCutSummary summary;
    summary.m_assignCount = program.m_assigns.size();
    if (program.m_assigns.empty()) return summary;

    DisjointSets dsu{program.m_assigns.size()};
    std::unordered_map<size_t, std::vector<size_t>> writersByVar;
    writersByVar.reserve(program.m_assigns.size());

    for (size_t assignIdx = 0; assignIdx < program.m_assigns.size(); ++assignIdx) {
        const V3SimAccelProgram::Assign& assign = program.m_assigns.at(assignIdx);
        writersByVar[assign.m_lhsIdx].push_back(assignIdx);
    }
    for (const auto& it : writersByVar) {
        const std::vector<size_t>& writers = it.second;
        for (size_t i = 1; i < writers.size(); ++i) dsu.unite(writers.front(), writers.at(i));
    }
    for (size_t assignIdx = 0; assignIdx < program.m_assigns.size(); ++assignIdx) {
        const V3SimAccelProgram::Assign& assign = program.m_assigns.at(assignIdx);
        for (const size_t rhsIdx : assign.m_rhsIdxs) {
            const auto wit = writersByVar.find(rhsIdx);
            if (wit == writersByVar.end()) continue;
            for (const size_t writerIdx : wit->second) dsu.unite(assignIdx, writerIdx);
        }
    }

    std::unordered_map<size_t, size_t> rootToClusterIdx;
    std::vector<TempCluster> clusters;
    clusters.reserve(program.m_assigns.size());
    auto clusterIndexOf = [&](size_t assignIdx) {
        const size_t root = dsu.find(assignIdx);
        const auto it = rootToClusterIdx.find(root);
        if (it != rootToClusterIdx.end()) return it->second;
        const size_t clusterIdx = clusters.size();
        clusters.emplace_back();
        rootToClusterIdx.emplace(root, clusterIdx);
        return clusterIdx;
    };

    for (size_t assignIdx = 0; assignIdx < program.m_assigns.size(); ++assignIdx) {
        const size_t clusterIdx = clusterIndexOf(assignIdx);
        TempCluster& cluster = clusters.at(clusterIdx);
        const V3SimAccelProgram::Assign& assign = program.m_assigns.at(assignIdx);
        const V3SimAccelProgram::Var& lhsVar = program.m_vars.at(assign.m_lhsIdx);
        ++cluster.m_assignCount;
        cluster.m_writtenVars.emplace(assign.m_lhsIdx);
        cluster.m_hierarchyCounts[lhsVar.m_hierarchy.empty() ? string{"<top>"} : lhsVar.m_hierarchy] += 1;
        for (const size_t rhsIdx : assign.m_rhsIdxs) cluster.m_readVars.emplace(rhsIdx);
    }

    std::unordered_map<size_t, std::unordered_set<size_t>> readerClustersByVar;
    std::unordered_map<size_t, std::unordered_set<size_t>> writerClustersByVar;
    for (size_t clusterIdx = 0; clusterIdx < clusters.size(); ++clusterIdx) {
        for (const size_t varIdx : clusters.at(clusterIdx).m_readVars) {
            readerClustersByVar[varIdx].emplace(clusterIdx);
        }
        for (const size_t varIdx : clusters.at(clusterIdx).m_writtenVars) {
            writerClustersByVar[varIdx].emplace(clusterIdx);
        }
    }

    std::unordered_set<size_t> uniqueBoundaryInputs;
    std::unordered_set<size_t> uniqueBoundaryOutputs;
    std::unordered_set<size_t> uniqueInternalVars;

    summary.m_clusterCount = clusters.size();
    for (size_t clusterIdx = 0; clusterIdx < clusters.size(); ++clusterIdx) {
        const TempCluster& cluster = clusters.at(clusterIdx);
        std::unordered_set<size_t> boundaryInputs;
        std::unordered_set<size_t> boundaryOutputs;
        std::unordered_set<size_t> internalVars;

        for (const size_t readVarIdx : cluster.m_readVars) {
            const auto wit = writerClustersByVar.find(readVarIdx);
            const bool writtenLocally
                = (wit != writerClustersByVar.end() && wit->second.find(clusterIdx) != wit->second.end());
            if (!writtenLocally) boundaryInputs.emplace(readVarIdx);
        }
        for (const size_t writtenVarIdx : cluster.m_writtenVars) {
            bool escapesCluster = false;
            const V3SimAccelProgram::Var& var = program.m_vars.at(writtenVarIdx);
            if (var.m_isGpuOutput || var.m_isCpuVisible) escapesCluster = true;
            const auto rit = readerClustersByVar.find(writtenVarIdx);
            if (rit != readerClustersByVar.end()) {
                for (const size_t readerClusterIdx : rit->second) {
                    if (readerClusterIdx != clusterIdx) {
                        escapesCluster = true;
                        break;
                    }
                }
            }
            if (escapesCluster) {
                boundaryOutputs.emplace(writtenVarIdx);
            } else {
                internalVars.emplace(writtenVarIdx);
            }
        }

        ApproxRegCutCluster clusterSummary;
        clusterSummary.m_assignCount = cluster.m_assignCount;
        clusterSummary.m_boundaryInputVarCount = boundaryInputs.size();
        clusterSummary.m_boundaryOutputVarCount = boundaryOutputs.size();
        clusterSummary.m_internalVarCount = internalVars.size();
        clusterSummary.m_boundaryInputBitCount = sumVarWidths(program, boundaryInputs);
        clusterSummary.m_boundaryOutputBitCount = sumVarWidths(program, boundaryOutputs);
        clusterSummary.m_internalBitCount = sumVarWidths(program, internalVars);
        for (const size_t varIdx : boundaryInputs) {
            if (program.m_vars.at(varIdx).m_isActivator) ++clusterSummary.m_activatorInputVarCount;
        }
        clusterSummary.m_uniqueHierarchyCount = cluster.m_hierarchyCounts.size();
        for (const auto& it : cluster.m_hierarchyCounts) {
            if (clusterSummary.m_dominantHierarchy.empty() || it.second > cluster.m_hierarchyCounts.at(clusterSummary.m_dominantHierarchy)
                || (it.second == cluster.m_hierarchyCounts.at(clusterSummary.m_dominantHierarchy)
                    && it.first < clusterSummary.m_dominantHierarchy)) {
                clusterSummary.m_dominantHierarchy = it.first;
            }
        }

        summary.m_boundaryInputVarCount += clusterSummary.m_boundaryInputVarCount;
        summary.m_boundaryOutputVarCount += clusterSummary.m_boundaryOutputVarCount;
        summary.m_internalVarCount += clusterSummary.m_internalVarCount;
        summary.m_boundaryInputBitCount += clusterSummary.m_boundaryInputBitCount;
        summary.m_boundaryOutputBitCount += clusterSummary.m_boundaryOutputBitCount;
        summary.m_internalBitCount += clusterSummary.m_internalBitCount;
        summary.m_activatorInputVarCount += clusterSummary.m_activatorInputVarCount;
        summary.m_maxAssignCount = std::max(summary.m_maxAssignCount, clusterSummary.m_assignCount);
        summary.m_maxBoundaryInputVarCount
            = std::max(summary.m_maxBoundaryInputVarCount, clusterSummary.m_boundaryInputVarCount);
        summary.m_maxBoundaryOutputVarCount
            = std::max(summary.m_maxBoundaryOutputVarCount, clusterSummary.m_boundaryOutputVarCount);
        summary.m_maxInternalVarCount
            = std::max(summary.m_maxInternalVarCount, clusterSummary.m_internalVarCount);

        uniqueBoundaryInputs.insert(boundaryInputs.begin(), boundaryInputs.end());
        uniqueBoundaryOutputs.insert(boundaryOutputs.begin(), boundaryOutputs.end());
        uniqueInternalVars.insert(internalVars.begin(), internalVars.end());
    }

    summary.m_uniqueBoundaryInputVarCount = uniqueBoundaryInputs.size();
    summary.m_uniqueBoundaryOutputVarCount = uniqueBoundaryOutputs.size();
    summary.m_uniqueInternalVarCount = uniqueInternalVars.size();
    return summary;
}
