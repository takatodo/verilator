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

#ifndef VERILATOR_V3SIMACCELPROGRAMANALYSIS_H_
#define VERILATOR_V3SIMACCELPROGRAMANALYSIS_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3SimAccelProgram.h"

#include <cstddef>
#include <cstdint>

class V3SimAccelProgramAnalysis final {
public:
    struct SpecFrontierCandidate final {
        size_t m_varIdx = 0;
        size_t m_assignUseCount = 0;
        size_t m_zeroBiasCount = 0;
        size_t m_oneBiasCount = 0;
        uint64_t m_specScore = 0;
        bool m_isActivator = false;
        int8_t m_preferredValue = -1;  // -1 unknown, 0 prefer zero, 1 prefer one
    };

    struct SpecFrontierSummary final {
        size_t m_candidateCount = 0;
        size_t m_clustersWithCandidates = 0;
        size_t m_preferredZeroCandidateCount = 0;
        size_t m_preferredOneCandidateCount = 0;
        size_t m_unknownPreferenceCandidateCount = 0;
        size_t m_activatorCandidateCount = 0;
        size_t m_maxCandidateCount = 0;
        uint64_t m_maxSpecScore = 0;
    };

    struct ApproxRegCutCluster final {
        size_t m_clusterIdx = 0;
        std::vector<size_t> m_assignIdxs;
        std::vector<size_t> m_boundaryInputVarIdxs;
        std::vector<size_t> m_boundaryOutputVarIdxs;
        std::vector<size_t> m_internalVarIdxs;
        std::vector<SpecFrontierCandidate> m_specFrontierCandidates;
        size_t m_assignCount = 0;
        size_t m_boundaryInputVarCount = 0;
        size_t m_boundaryOutputVarCount = 0;
        size_t m_internalVarCount = 0;
        size_t m_specFrontierCandidateCount = 0;
        size_t m_specFrontierPreferredZeroCount = 0;
        size_t m_specFrontierPreferredOneCount = 0;
        size_t m_specFrontierUnknownPreferenceCount = 0;
        uint64_t m_boundaryInputBitCount = 0;
        uint64_t m_boundaryOutputBitCount = 0;
        uint64_t m_internalBitCount = 0;
        uint64_t m_specFrontierMaxScore = 0;
        size_t m_activatorInputVarCount = 0;
        string m_dominantHierarchy;
        size_t m_uniqueHierarchyCount = 0;
    };

    struct ApproxRegCutSummary final {
        size_t m_clusterCount = 0;
        size_t m_assignCount = 0;
        size_t m_boundaryInputVarCount = 0;
        size_t m_boundaryOutputVarCount = 0;
        size_t m_internalVarCount = 0;
        size_t m_uniqueBoundaryInputVarCount = 0;
        size_t m_uniqueBoundaryOutputVarCount = 0;
        size_t m_uniqueInternalVarCount = 0;
        uint64_t m_boundaryInputBitCount = 0;
        uint64_t m_boundaryOutputBitCount = 0;
        uint64_t m_internalBitCount = 0;
        size_t m_activatorInputVarCount = 0;
        size_t m_maxAssignCount = 0;
        size_t m_maxBoundaryInputVarCount = 0;
        size_t m_maxBoundaryOutputVarCount = 0;
        size_t m_maxInternalVarCount = 0;
    };

    struct ApproxRegCutAnalysis final {
        ApproxRegCutSummary m_summary;
        SpecFrontierSummary m_specFrontierSummary;
        std::vector<ApproxRegCutCluster> m_clusters;
    };

    static ApproxRegCutAnalysis analyzeApproxRegCut(const V3SimAccelProgram& program);
};

#endif  // Guard
