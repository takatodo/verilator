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
#include <array>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr size_t kExprKindCount
    = static_cast<size_t>(V3SimAccelProgram::ExprKind::EXTENDS) + 1U;

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
    std::vector<size_t> m_assignIdxs;
    std::unordered_set<size_t> m_readVars;
    std::unordered_set<size_t> m_writtenVars;
    std::unordered_map<string, size_t> m_hierarchyCounts;
    size_t m_assignCount = 0;
};

struct TempSpecCandidate final {
    size_t m_varIdx = 0;
    size_t m_assignUseCount = 0;
    size_t m_zeroBiasCount = 0;
    size_t m_oneBiasCount = 0;
    bool m_isActivator = false;
};

enum class SpecBias : uint8_t { NONE, ZERO, ONE };

bool isOperatorExprKind(V3SimAccelProgram::ExprKind kind) {
    return kind != V3SimAccelProgram::ExprKind::VAR
           && kind != V3SimAccelProgram::ExprKind::CONST;
}

uint64_t sumVarWidths(const V3SimAccelProgram& program, const std::unordered_set<size_t>& varIdxs) {
    uint64_t total = 0;
    for (const size_t varIdx : varIdxs) total += program.m_vars.at(varIdx).m_width;
    return total;
}

std::vector<size_t> sortedVectorOf(const std::unordered_set<size_t>& varIdxs) {
    std::vector<size_t> out{varIdxs.begin(), varIdxs.end()};
    std::sort(out.begin(), out.end());
    return out;
}

void accumulateSpecBias(const V3SimAccelProgram& program, size_t exprIdx,
                        const std::unordered_set<size_t>& frontierVarIdxs,
                        std::unordered_map<size_t, TempSpecCandidate>& candidates,
                        SpecBias inheritedBias = SpecBias::NONE) {
    if (exprIdx == V3SimAccelProgram::INVALID_SLOT) return;
    const V3SimAccelProgram::Expr& expr = program.m_exprs.at(exprIdx);
    switch (expr.m_kind) {
    case V3SimAccelProgram::ExprKind::VAR: {
        if (expr.m_varIdx == V3SimAccelProgram::INVALID_SLOT) return;
        if (frontierVarIdxs.find(expr.m_varIdx) == frontierVarIdxs.end()) return;
        TempSpecCandidate& candidate = candidates[expr.m_varIdx];
        candidate.m_varIdx = expr.m_varIdx;
        if (inheritedBias == SpecBias::ZERO) {
            ++candidate.m_zeroBiasCount;
        } else if (inheritedBias == SpecBias::ONE) {
            ++candidate.m_oneBiasCount;
        }
        return;
    }
    case V3SimAccelProgram::ExprKind::CONST: return;
    case V3SimAccelProgram::ExprKind::AND:
    case V3SimAccelProgram::ExprKind::LOGAND:
        accumulateSpecBias(program, expr.m_lhs, frontierVarIdxs, candidates, SpecBias::ZERO);
        accumulateSpecBias(program, expr.m_rhs, frontierVarIdxs, candidates, SpecBias::ZERO);
        return;
    case V3SimAccelProgram::ExprKind::OR:
    case V3SimAccelProgram::ExprKind::LOGOR:
        accumulateSpecBias(program, expr.m_lhs, frontierVarIdxs, candidates, SpecBias::ONE);
        accumulateSpecBias(program, expr.m_rhs, frontierVarIdxs, candidates, SpecBias::ONE);
        return;
    case V3SimAccelProgram::ExprKind::COND:
        // A true/active condition is usually the speculation-friendly side to precompute first.
        accumulateSpecBias(program, expr.m_lhs, frontierVarIdxs, candidates, SpecBias::ONE);
        accumulateSpecBias(program, expr.m_rhs, frontierVarIdxs, candidates, SpecBias::NONE);
        accumulateSpecBias(program, expr.m_third, frontierVarIdxs, candidates, SpecBias::NONE);
        return;
    case V3SimAccelProgram::ExprKind::CCAST:
    case V3SimAccelProgram::ExprKind::EXTEND:
    case V3SimAccelProgram::ExprKind::EXTENDS:
        accumulateSpecBias(program, expr.m_lhs, frontierVarIdxs, candidates, inheritedBias);
        return;
    case V3SimAccelProgram::ExprKind::XOR:
    case V3SimAccelProgram::ExprKind::ADD:
    case V3SimAccelProgram::ExprKind::SUB:
    case V3SimAccelProgram::ExprKind::EQ:
    case V3SimAccelProgram::ExprKind::NEQ:
    case V3SimAccelProgram::ExprKind::CONCAT:
    case V3SimAccelProgram::ExprKind::SEL:
    case V3SimAccelProgram::ExprKind::SHIFTL:
    case V3SimAccelProgram::ExprKind::SHIFTLOVR:
    case V3SimAccelProgram::ExprKind::SHIFTR:
    case V3SimAccelProgram::ExprKind::SHIFTROVR:
    case V3SimAccelProgram::ExprKind::NOT:
    case V3SimAccelProgram::ExprKind::LOGNOT:
        accumulateSpecBias(program, expr.m_lhs, frontierVarIdxs, candidates, SpecBias::NONE);
        accumulateSpecBias(program, expr.m_rhs, frontierVarIdxs, candidates, SpecBias::NONE);
        accumulateSpecBias(program, expr.m_third, frontierVarIdxs, candidates, SpecBias::NONE);
        return;
    }
}

void accumulateExprKinds(const V3SimAccelProgram& program, size_t exprIdx,
                         std::unordered_set<size_t>& visitedExprIdxs,
                         std::vector<size_t>& exprKindCounts, size_t& operatorCount) {
    if (exprIdx == V3SimAccelProgram::INVALID_SLOT) return;
    if (!visitedExprIdxs.emplace(exprIdx).second) return;
    const V3SimAccelProgram::Expr& expr = program.m_exprs.at(exprIdx);
    ++exprKindCounts.at(static_cast<size_t>(expr.m_kind));
    if (isOperatorExprKind(expr.m_kind)) ++operatorCount;
    if (expr.m_kind == V3SimAccelProgram::ExprKind::VAR
        || expr.m_kind == V3SimAccelProgram::ExprKind::CONST) {
        return;
    }
    accumulateExprKinds(program, expr.m_lhs, visitedExprIdxs, exprKindCounts, operatorCount);
    accumulateExprKinds(program, expr.m_rhs, visitedExprIdxs, exprKindCounts, operatorCount);
    accumulateExprKinds(program, expr.m_third, visitedExprIdxs, exprKindCounts, operatorCount);
}

uint64_t computeSpecScore(const TempSpecCandidate& candidate) {
    const uint64_t biasCount
        = std::max(candidate.m_zeroBiasCount, candidate.m_oneBiasCount);
    return static_cast<uint64_t>(candidate.m_assignUseCount) * 4ULL + biasCount * 16ULL
           + (candidate.m_isActivator ? 32ULL : 0ULL);
}

}  // namespace

V3SimAccelProgramAnalysis::ApproxRegCutAnalysis
V3SimAccelProgramAnalysis::analyzeApproxRegCut(const V3SimAccelProgram& program) {
    ApproxRegCutAnalysis analysis;
    ApproxRegCutSummary& summary = analysis.m_summary;
    V3SimAccelProgramAnalysis::SpecFrontierSummary& frontierSummary
        = analysis.m_specFrontierSummary;
    summary.m_assignCount = program.m_assigns.size();
    summary.m_exprKindCounts.assign(kExprKindCount, 0);
    if (program.m_assigns.empty()) return analysis;

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
        cluster.m_assignIdxs.push_back(assignIdx);
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
    analysis.m_clusters.reserve(clusters.size());
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
        clusterSummary.m_clusterIdx = clusterIdx;
        clusterSummary.m_exprKindCounts.assign(kExprKindCount, 0);
        clusterSummary.m_assignCount = cluster.m_assignCount;
        clusterSummary.m_inputSignatureVarCount = boundaryInputs.size();
        clusterSummary.m_boundaryInputVarCount = boundaryInputs.size();
        clusterSummary.m_boundaryOutputVarCount = boundaryOutputs.size();
        clusterSummary.m_internalVarCount = internalVars.size();
        clusterSummary.m_inputSignatureBitCount = sumVarWidths(program, boundaryInputs);
        clusterSummary.m_boundaryInputBitCount = sumVarWidths(program, boundaryInputs);
        clusterSummary.m_boundaryOutputBitCount = sumVarWidths(program, boundaryOutputs);
        clusterSummary.m_internalBitCount = sumVarWidths(program, internalVars);
        clusterSummary.m_boundaryInputVarIdxs = sortedVectorOf(boundaryInputs);
        clusterSummary.m_boundaryOutputVarIdxs = sortedVectorOf(boundaryOutputs);
        clusterSummary.m_internalVarIdxs = sortedVectorOf(internalVars);
        for (const size_t varIdx : boundaryInputs) {
            if (program.m_vars.at(varIdx).m_isActivator) ++clusterSummary.m_activatorInputVarCount;
            const auto wit = writerClustersByVar.find(varIdx);
            if (wit == writerClustersByVar.end() || wit->second.empty()) {
                clusterSummary.m_cpuBoundaryInputVarIdxs.push_back(varIdx);
            } else {
                clusterSummary.m_gpuInternalInputVarIdxs.push_back(varIdx);
            }
        }
        for (const size_t varIdx : boundaryOutputs) {
            const V3SimAccelProgram::Var& var = program.m_vars.at(varIdx);
            if (var.m_isGpuOutput || var.m_isCpuVisible) {
                clusterSummary.m_cpuBoundaryOutputVarIdxs.push_back(varIdx);
            } else {
                clusterSummary.m_gpuInternalOutputVarIdxs.push_back(varIdx);
            }
        }
        std::sort(clusterSummary.m_cpuBoundaryInputVarIdxs.begin(),
                  clusterSummary.m_cpuBoundaryInputVarIdxs.end());
        std::sort(clusterSummary.m_gpuInternalInputVarIdxs.begin(),
                  clusterSummary.m_gpuInternalInputVarIdxs.end());
        std::sort(clusterSummary.m_cpuBoundaryOutputVarIdxs.begin(),
                  clusterSummary.m_cpuBoundaryOutputVarIdxs.end());
        std::sort(clusterSummary.m_gpuInternalOutputVarIdxs.begin(),
                  clusterSummary.m_gpuInternalOutputVarIdxs.end());
        clusterSummary.m_uniqueHierarchyCount = cluster.m_hierarchyCounts.size();
        for (const auto& it : cluster.m_hierarchyCounts) {
            if (clusterSummary.m_dominantHierarchy.empty() || it.second > cluster.m_hierarchyCounts.at(clusterSummary.m_dominantHierarchy)
                || (it.second == cluster.m_hierarchyCounts.at(clusterSummary.m_dominantHierarchy)
                    && it.first < clusterSummary.m_dominantHierarchy)) {
                clusterSummary.m_dominantHierarchy = it.first;
            }
        }
        clusterSummary.m_assignIdxs = cluster.m_assignIdxs;

        std::unordered_set<size_t> visitedExprIdxs;
        for (const size_t assignIdx : cluster.m_assignIdxs) {
            if (assignIdx >= program.m_assigns.size()) continue;
            accumulateExprKinds(program, program.m_assigns.at(assignIdx).m_exprIdx, visitedExprIdxs,
                                clusterSummary.m_exprKindCounts,
                                clusterSummary.m_operatorCount);
        }

        std::unordered_set<size_t> frontierVarIdxs{boundaryInputs.begin(), boundaryInputs.end()};
        std::unordered_map<size_t, TempSpecCandidate> tempCandidates;
        tempCandidates.reserve(frontierVarIdxs.size());
        for (const size_t varIdx : frontierVarIdxs) {
            TempSpecCandidate& candidate = tempCandidates[varIdx];
            candidate.m_varIdx = varIdx;
            candidate.m_isActivator = program.m_vars.at(varIdx).m_isActivator;
        }
        for (const size_t assignIdx : cluster.m_assignIdxs) {
            const V3SimAccelProgram::Assign& assign = program.m_assigns.at(assignIdx);
            for (const size_t rhsIdx : assign.m_rhsIdxs) {
                const auto it = tempCandidates.find(rhsIdx);
                if (it != tempCandidates.end()) ++it->second.m_assignUseCount;
            }
            accumulateSpecBias(program, assign.m_exprIdx, frontierVarIdxs, tempCandidates);
        }

        std::vector<V3SimAccelProgramAnalysis::SpecFrontierCandidate> specCandidates;
        specCandidates.reserve(tempCandidates.size());
        for (const auto& it : tempCandidates) {
            const TempSpecCandidate& temp = it.second;
            V3SimAccelProgramAnalysis::SpecFrontierCandidate candidate;
            candidate.m_varIdx = temp.m_varIdx;
            candidate.m_assignUseCount = temp.m_assignUseCount;
            candidate.m_zeroBiasCount = temp.m_zeroBiasCount;
            candidate.m_oneBiasCount = temp.m_oneBiasCount;
            candidate.m_isActivator = temp.m_isActivator;
            candidate.m_specScore = computeSpecScore(temp);
            if (temp.m_zeroBiasCount > temp.m_oneBiasCount) {
                candidate.m_preferredValue = 0;
            } else if (temp.m_oneBiasCount > temp.m_zeroBiasCount) {
                candidate.m_preferredValue = 1;
            } else if (temp.m_isActivator) {
                candidate.m_preferredValue = 1;
            } else {
                candidate.m_preferredValue = -1;
            }
            specCandidates.push_back(std::move(candidate));
        }
        std::sort(specCandidates.begin(), specCandidates.end(),
                  [](const V3SimAccelProgramAnalysis::SpecFrontierCandidate& lhs,
                     const V3SimAccelProgramAnalysis::SpecFrontierCandidate& rhs) {
                      if (lhs.m_specScore != rhs.m_specScore) {
                          return lhs.m_specScore > rhs.m_specScore;
                      }
                      if (lhs.m_preferredValue != rhs.m_preferredValue) {
                          return lhs.m_preferredValue > rhs.m_preferredValue;
                      }
                      if (lhs.m_assignUseCount != rhs.m_assignUseCount) {
                          return lhs.m_assignUseCount > rhs.m_assignUseCount;
                      }
                      return lhs.m_varIdx < rhs.m_varIdx;
                  });
        clusterSummary.m_specFrontierCandidates = std::move(specCandidates);
        clusterSummary.m_specFrontierCandidateCount = clusterSummary.m_specFrontierCandidates.size();
        for (const V3SimAccelProgramAnalysis::SpecFrontierCandidate& candidate :
             clusterSummary.m_specFrontierCandidates) {
            frontierSummary.m_maxSpecScore
                = std::max(frontierSummary.m_maxSpecScore, candidate.m_specScore);
            clusterSummary.m_specFrontierMaxScore
                = std::max(clusterSummary.m_specFrontierMaxScore, candidate.m_specScore);
            if (candidate.m_isActivator) ++frontierSummary.m_activatorCandidateCount;
            if (candidate.m_preferredValue == 0) {
                ++clusterSummary.m_specFrontierPreferredZeroCount;
                ++frontierSummary.m_preferredZeroCandidateCount;
            } else if (candidate.m_preferredValue == 1) {
                ++clusterSummary.m_specFrontierPreferredOneCount;
                ++frontierSummary.m_preferredOneCandidateCount;
            } else {
                ++clusterSummary.m_specFrontierUnknownPreferenceCount;
                ++frontierSummary.m_unknownPreferenceCandidateCount;
            }
        }
        frontierSummary.m_candidateCount += clusterSummary.m_specFrontierCandidateCount;
        if (clusterSummary.m_specFrontierCandidateCount) ++frontierSummary.m_clustersWithCandidates;
        frontierSummary.m_maxCandidateCount
            = std::max(frontierSummary.m_maxCandidateCount,
                       clusterSummary.m_specFrontierCandidateCount);

        if (!clusterSummary.m_cpuBoundaryOutputVarIdxs.empty()
            || clusterSummary.m_inputSignatureBitCount > 128
            || (clusterSummary.m_boundaryInputVarCount > 0
                && clusterSummary.m_cpuBoundaryInputVarIdxs.size() * 2
                       >= clusterSummary.m_boundaryInputVarCount)) {
            clusterSummary.m_hybridOwnerHint = "cpu_boundary_heavy";
            ++analysis.m_cpuBoundaryHeavyClusterCount;
        } else {
            clusterSummary.m_hybridOwnerHint = "gpu_candidate";
            ++analysis.m_gpuCandidateClusterCount;
        }

        summary.m_inputSignatureVarCount += clusterSummary.m_inputSignatureVarCount;
        summary.m_operatorCount += clusterSummary.m_operatorCount;
        summary.m_boundaryInputVarCount += clusterSummary.m_boundaryInputVarCount;
        summary.m_boundaryOutputVarCount += clusterSummary.m_boundaryOutputVarCount;
        summary.m_internalVarCount += clusterSummary.m_internalVarCount;
        summary.m_inputSignatureBitCount += clusterSummary.m_inputSignatureBitCount;
        summary.m_boundaryInputBitCount += clusterSummary.m_boundaryInputBitCount;
        summary.m_boundaryOutputBitCount += clusterSummary.m_boundaryOutputBitCount;
        summary.m_internalBitCount += clusterSummary.m_internalBitCount;
        summary.m_activatorInputVarCount += clusterSummary.m_activatorInputVarCount;
        summary.m_maxAssignCount = std::max(summary.m_maxAssignCount, clusterSummary.m_assignCount);
        summary.m_maxInputSignatureVarCount
            = std::max(summary.m_maxInputSignatureVarCount, clusterSummary.m_inputSignatureVarCount);
        summary.m_maxBoundaryInputVarCount
            = std::max(summary.m_maxBoundaryInputVarCount, clusterSummary.m_boundaryInputVarCount);
        summary.m_maxBoundaryOutputVarCount
            = std::max(summary.m_maxBoundaryOutputVarCount, clusterSummary.m_boundaryOutputVarCount);
        summary.m_maxInternalVarCount
            = std::max(summary.m_maxInternalVarCount, clusterSummary.m_internalVarCount);
        summary.m_maxInputSignatureBitCount
            = std::max(summary.m_maxInputSignatureBitCount, clusterSummary.m_inputSignatureBitCount);
        for (size_t kindIdx = 0; kindIdx < clusterSummary.m_exprKindCounts.size(); ++kindIdx) {
            summary.m_exprKindCounts.at(kindIdx) += clusterSummary.m_exprKindCounts.at(kindIdx);
        }

        uniqueBoundaryInputs.insert(boundaryInputs.begin(), boundaryInputs.end());
        uniqueBoundaryOutputs.insert(boundaryOutputs.begin(), boundaryOutputs.end());
        uniqueInternalVars.insert(internalVars.begin(), internalVars.end());
        analysis.m_clusters.push_back(std::move(clusterSummary));
    }

    std::vector<std::unordered_set<size_t>> dependencySets(analysis.m_clusters.size());
    std::vector<std::unordered_set<size_t>> dependentSets(analysis.m_clusters.size());
    for (size_t clusterIdx = 0; clusterIdx < analysis.m_clusters.size(); ++clusterIdx) {
        for (const size_t varIdx : analysis.m_clusters.at(clusterIdx).m_boundaryInputVarIdxs) {
            const auto wit = writerClustersByVar.find(varIdx);
            if (wit == writerClustersByVar.end()) continue;
            for (const size_t producerIdx : wit->second) {
                if (producerIdx == clusterIdx) continue;
                dependencySets.at(clusterIdx).emplace(producerIdx);
                dependentSets.at(producerIdx).emplace(clusterIdx);
            }
        }
    }

    std::vector<size_t> indegree(analysis.m_clusters.size(), 0);
    for (size_t clusterIdx = 0; clusterIdx < analysis.m_clusters.size(); ++clusterIdx) {
        indegree.at(clusterIdx) = dependencySets.at(clusterIdx).size();
        analysis.m_clusters.at(clusterIdx).m_dependencyClusterIdxs
            = sortedVectorOf(dependencySets.at(clusterIdx));
        analysis.m_clusters.at(clusterIdx).m_dependentClusterIdxs
            = sortedVectorOf(dependentSets.at(clusterIdx));
    }
    std::set<size_t> ready;
    for (size_t clusterIdx = 0; clusterIdx < indegree.size(); ++clusterIdx) {
        if (indegree.at(clusterIdx) == 0) ready.insert(clusterIdx);
    }
    while (!ready.empty()) {
        const size_t clusterIdx = *ready.begin();
        ready.erase(ready.begin());
        analysis.m_clusters.at(clusterIdx).m_topoRank = analysis.m_clusterTopoOrder.size();
        analysis.m_clusterTopoOrder.push_back(clusterIdx);
        for (const size_t dependentIdx : analysis.m_clusters.at(clusterIdx).m_dependentClusterIdxs) {
            if (indegree.at(dependentIdx) == 0) continue;
            --indegree.at(dependentIdx);
            if (indegree.at(dependentIdx) == 0) ready.insert(dependentIdx);
        }
    }
    if (analysis.m_clusterTopoOrder.size() != analysis.m_clusters.size()) {
        analysis.m_clusterTopoOrder.clear();
        for (size_t clusterIdx = 0; clusterIdx < analysis.m_clusters.size(); ++clusterIdx) {
            analysis.m_clusters.at(clusterIdx).m_topoRank = clusterIdx;
            analysis.m_clusterTopoOrder.push_back(clusterIdx);
        }
    }

    summary.m_uniqueBoundaryInputVarCount = uniqueBoundaryInputs.size();
    summary.m_uniqueBoundaryOutputVarCount = uniqueBoundaryOutputs.size();
    summary.m_uniqueInternalVarCount = uniqueInternalVars.size();
    return analysis;
}
