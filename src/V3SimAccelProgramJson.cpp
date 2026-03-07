// DESCRIPTION: Verilator: Sim-Accel Program JSON Writer
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

#include "V3SimAccelProgramJson.h"

#include "V3Error.h"
#include "V3SimAccelProgramAnalysis.h"

#include <fstream>

namespace {

string jsonEscape(const string& in) {
    string out;
    out.reserve(in.size() + 8);
    for (const char ch : in) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += ch; break;
        }
    }
    return out;
}

const char* exprKindName(V3SimAccelProgram::ExprKind kind) {
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

void writeIndexOrNull(std::ofstream& of, size_t value) {
    if (value == V3SimAccelProgram::INVALID_SLOT) {
        of << "null";
    } else {
        of << value;
    }
}

void writePreferredValue(std::ofstream& of, int8_t value) {
    if (value < 0) {
        of << "null";
    } else {
        of << static_cast<int>(value);
    }
}

void writeVarIndexList(std::ofstream& of, const std::vector<size_t>& values, const char* indent) {
    of << "[";
    if (!values.empty()) of << "\n";
    for (size_t i = 0; i < values.size(); ++i) {
        of << indent << values.at(i);
        if (i + 1 != values.size()) of << ",";
        of << "\n";
    }
    if (!values.empty()) of << "  ";
    of << "]";
}

void writeExprKindCounts(std::ofstream& of, const std::vector<size_t>& counts, const char* indent,
                         const char* closingIndent) {
    of << "{\n";
    for (size_t kindIdx = 0; kindIdx < counts.size(); ++kindIdx) {
        const auto kind = static_cast<V3SimAccelProgram::ExprKind>(kindIdx);
        of << indent << "\"" << exprKindName(kind) << "\": " << counts.at(kindIdx);
        if (kindIdx + 1 != counts.size()) of << ",";
        of << "\n";
    }
    of << closingIndent << "}";
}

}  // namespace

void V3SimAccelProgramJson::write(const string& filename, const V3SimAccelProgram& program,
                                  const string& strategy) {
    const V3SimAccelProgramAnalysis::ApproxRegCutAnalysis approxRegCut
        = V3SimAccelProgramAnalysis::analyzeApproxRegCut(program);
    const V3SimAccelProgramAnalysis::ApproxRegCutSummary& approxRegCutSummary
        = approxRegCut.m_summary;
    const V3SimAccelProgramAnalysis::SpecFrontierSummary& specFrontierSummary
        = approxRegCut.m_specFrontierSummary;
    std::ofstream of{filename};
    if (!of.is_open()) v3fatal("Cannot open output file: " + filename);  // LCOV_EXCL_LINE

    of << "{\n";
    of << "  \"format\": \"sim-accel-program-v1\",\n";
    of << "  \"strategy\": \"" << jsonEscape(strategy) << "\",\n";

    of << "  \"stats\": {\n";
    of << "    \"assignw_supported\": " << program.m_stats.m_assignwSupported << ",\n";
    of << "    \"assignw_total\": " << program.m_stats.m_assignwTotal << ",\n";
    of << "    \"assignw_ignored\": " << program.m_stats.m_assignwIgnored << "\n";
    of << "  },\n";

    of << "  \"vars\": [\n";
    for (size_t i = 0; i < program.m_vars.size(); ++i) {
        const V3SimAccelProgram::Var& var = program.m_vars.at(i);
        of << "    {\n";
        of << "      \"index\": " << i << ",\n";
        of << "      \"name\": \"" << jsonEscape(var.m_name) << "\",\n";
        of << "      \"hierarchy\": \"" << jsonEscape(var.m_hierarchy) << "\",\n";
        of << "      \"direction\": \"" << jsonEscape(var.m_direction) << "\",\n";
        of << "      \"width\": " << var.m_width << ",\n";
        of << "      \"is_primary_io\": " << (var.m_isPrimaryIo ? "true" : "false") << ",\n";
        of << "      \"is_activator\": " << (var.m_isActivator ? "true" : "false") << ",\n";
        of << "      \"is_cpu_visible\": " << (var.m_isCpuVisible ? "true" : "false") << ",\n";
        of << "      \"is_gpu_input\": " << (var.m_isGpuInput ? "true" : "false") << ",\n";
        of << "      \"is_gpu_output\": " << (var.m_isGpuOutput ? "true" : "false") << ",\n";
        of << "      \"input_slot\": ";
        writeIndexOrNull(of, var.m_inputSlot);
        of << ",\n";
        of << "      \"output_slot\": ";
        writeIndexOrNull(of, var.m_outputSlot);
        of << "\n";
        of << "    }";
        if (i + 1 != program.m_vars.size()) of << ",";
        of << "\n";
    }
    of << "  ],\n";

    of << "  \"exprs\": [\n";
    for (size_t i = 0; i < program.m_exprs.size(); ++i) {
        const V3SimAccelProgram::Expr& expr = program.m_exprs.at(i);
        of << "    {\n";
        of << "      \"index\": " << i << ",\n";
        of << "      \"kind\": \"" << exprKindName(expr.m_kind) << "\",\n";
        of << "      \"width\": " << expr.m_width << ",\n";
        of << "      \"lhs\": ";
        writeIndexOrNull(of, expr.m_lhs);
        of << ",\n";
        of << "      \"rhs\": ";
        writeIndexOrNull(of, expr.m_rhs);
        of << ",\n";
        of << "      \"third\": ";
        writeIndexOrNull(of, expr.m_third);
        of << ",\n";
        of << "      \"var_idx\": ";
        writeIndexOrNull(of, expr.m_varIdx);
        of << ",\n";
        of << "      \"const_value\": " << expr.m_constValue << "\n";
        of << "    }";
        if (i + 1 != program.m_exprs.size()) of << ",";
        of << "\n";
    }
    of << "  ],\n";

    of << "  \"assigns\": [\n";
    for (size_t i = 0; i < program.m_assigns.size(); ++i) {
        const V3SimAccelProgram::Assign& assign = program.m_assigns.at(i);
        of << "    {\n";
        of << "      \"lhs_idx\": " << assign.m_lhsIdx << ",\n";
        of << "      \"lhs_name\": \"" << jsonEscape(assign.m_lhsName) << "\",\n";
        of << "      \"expr_idx\": " << assign.m_exprIdx << ",\n";
        of << "      \"rhs_idx_list\": [";
        for (size_t j = 0; j < assign.m_rhsIdxs.size(); ++j) {
            if (j) of << ", ";
            of << assign.m_rhsIdxs.at(j);
        }
        of << "]\n";
        of << "    }";
        if (i + 1 != program.m_assigns.size()) of << ",";
        of << "\n";
    }
    of << "  ],\n";

    of << "  \"comm_buffers\": {\n";
    of << "    \"cpu_to_gpu_var_idxs\": ";
    writeVarIndexList(of, program.m_commPlan.m_cpuToGpuVarIdxs, "      ");
    of << ",\n";
    of << "    \"gpu_to_cpu_var_idxs\": ";
    writeVarIndexList(of, program.m_commPlan.m_gpuToCpuVarIdxs, "      ");
    of << ",\n";
    of << "    \"cpu_visible_var_idxs\": ";
    writeVarIndexList(of, program.m_commPlan.m_cpuVisibleVarIdxs, "      ");
    of << "\n";
    of << "  },\n";

    of << "  \"preload_targets\": [\n";
    for (size_t i = 0; i < program.m_preloadTargets.size(); ++i) {
        const V3SimAccelProgram::PreloadTarget& target = program.m_preloadTargets.at(i);
        of << "    {\n";
        of << "      \"kind\": \"" << jsonEscape(target.m_kind) << "\",\n";
        of << "      \"name\": \"" << jsonEscape(target.m_name) << "\",\n";
        of << "      \"target_path\": \"" << jsonEscape(target.m_targetPath) << "\",\n";
        of << "      \"ast_name\": \"" << jsonEscape(target.m_astName) << "\",\n";
        of << "      \"hierarchy\": \"" << jsonEscape(target.m_hierarchy) << "\",\n";
        of << "      \"word_bits\": " << target.m_wordBits << ",\n";
        of << "      \"depth\": " << target.m_depth << ",\n";
        of << "      \"base_addr\": " << target.m_baseAddr << ",\n";
        of << "      \"address_unit_bytes\": " << target.m_addressUnitBytes << ",\n";
        of << "      \"endianness\": \"" << jsonEscape(target.m_endianness) << "\",\n";
        of << "      \"is_primary_io\": " << (target.m_isPrimaryIo ? "true" : "false")
           << "\n";
        of << "    }";
        if (i + 1 != program.m_preloadTargets.size()) of << ",";
        of << "\n";
    }
    of << "  ],\n";

    of << "  \"approx_regcut_analysis\": {\n";
    of << "    \"cluster_count\": " << approxRegCutSummary.m_clusterCount << ",\n";
    of << "    \"assign_count\": " << approxRegCutSummary.m_assignCount << ",\n";
    of << "    \"operator_count\": " << approxRegCutSummary.m_operatorCount << ",\n";
    of << "    \"input_signature_var_count\": " << approxRegCutSummary.m_inputSignatureVarCount
       << ",\n";
    of << "    \"input_signature_bit_count\": " << approxRegCutSummary.m_inputSignatureBitCount
       << ",\n";
    of << "    \"boundary_input_var_count\": " << approxRegCutSummary.m_boundaryInputVarCount
       << ",\n";
    of << "    \"boundary_output_var_count\": " << approxRegCutSummary.m_boundaryOutputVarCount
       << ",\n";
    of << "    \"internal_var_count\": " << approxRegCutSummary.m_internalVarCount << ",\n";
    of << "    \"unique_boundary_input_var_count\": "
       << approxRegCutSummary.m_uniqueBoundaryInputVarCount << ",\n";
    of << "    \"unique_boundary_output_var_count\": "
       << approxRegCutSummary.m_uniqueBoundaryOutputVarCount << ",\n";
    of << "    \"unique_internal_var_count\": " << approxRegCutSummary.m_uniqueInternalVarCount
       << ",\n";
    of << "    \"boundary_input_bit_count\": " << approxRegCutSummary.m_boundaryInputBitCount
       << ",\n";
    of << "    \"boundary_output_bit_count\": " << approxRegCutSummary.m_boundaryOutputBitCount
       << ",\n";
    of << "    \"internal_bit_count\": " << approxRegCutSummary.m_internalBitCount << ",\n";
    of << "    \"activator_input_var_count\": " << approxRegCutSummary.m_activatorInputVarCount
       << ",\n";
    of << "    \"max_assign_count\": " << approxRegCutSummary.m_maxAssignCount << ",\n";
    of << "    \"max_input_signature_var_count\": "
       << approxRegCutSummary.m_maxInputSignatureVarCount << ",\n";
    of << "    \"max_input_signature_bit_count\": "
       << approxRegCutSummary.m_maxInputSignatureBitCount << ",\n";
    of << "    \"max_boundary_input_var_count\": "
       << approxRegCutSummary.m_maxBoundaryInputVarCount << ",\n";
    of << "    \"max_boundary_output_var_count\": "
       << approxRegCutSummary.m_maxBoundaryOutputVarCount << ",\n";
    of << "    \"max_internal_var_count\": " << approxRegCutSummary.m_maxInternalVarCount
       << ",\n";
    of << "    \"expr_kind_counts\": ";
    writeExprKindCounts(of, approxRegCutSummary.m_exprKindCounts, "      ", "    ");
    of << "\n";
    of << "  },\n";

    of << "  \"spec_frontier_analysis\": {\n";
    of << "    \"candidate_count\": " << specFrontierSummary.m_candidateCount << ",\n";
    of << "    \"clusters_with_candidates\": " << specFrontierSummary.m_clustersWithCandidates
       << ",\n";
    of << "    \"preferred_zero_candidate_count\": "
       << specFrontierSummary.m_preferredZeroCandidateCount << ",\n";
    of << "    \"preferred_one_candidate_count\": "
       << specFrontierSummary.m_preferredOneCandidateCount << ",\n";
    of << "    \"unknown_preference_candidate_count\": "
       << specFrontierSummary.m_unknownPreferenceCandidateCount << ",\n";
    of << "    \"activator_candidate_count\": " << specFrontierSummary.m_activatorCandidateCount
       << ",\n";
    of << "    \"max_candidate_count\": " << specFrontierSummary.m_maxCandidateCount << ",\n";
    of << "    \"max_spec_score\": " << specFrontierSummary.m_maxSpecScore << "\n";
    of << "  },\n";

    of << "  \"hybrid_cluster_analysis\": {\n";
    of << "    \"cluster_count\": " << approxRegCut.m_clusters.size() << ",\n";
    of << "    \"gpu_candidate_cluster_count\": " << approxRegCut.m_gpuCandidateClusterCount
       << ",\n";
    of << "    \"cpu_boundary_heavy_cluster_count\": "
       << approxRegCut.m_cpuBoundaryHeavyClusterCount << ",\n";
    of << "    \"cpu_only_blocked_cluster_count\": "
       << approxRegCut.m_cpuOnlyBlockedClusterCount << ",\n";
    of << "    \"cluster_topo_order\": ";
    writeVarIndexList(of, approxRegCut.m_clusterTopoOrder, "      ");
    of << "\n";
    of << "  },\n";

    of << "  \"approx_regcut_clusters\": [\n";
    for (size_t i = 0; i < approxRegCut.m_clusters.size(); ++i) {
        const V3SimAccelProgramAnalysis::ApproxRegCutCluster& cluster
            = approxRegCut.m_clusters.at(i);
        of << "    {\n";
        of << "      \"cluster_idx\": " << cluster.m_clusterIdx << ",\n";
        of << "      \"assign_count\": " << cluster.m_assignCount << ",\n";
        of << "      \"operator_count\": " << cluster.m_operatorCount << ",\n";
        of << "      \"input_signature_var_count\": " << cluster.m_inputSignatureVarCount << ",\n";
        of << "      \"boundary_input_var_count\": " << cluster.m_boundaryInputVarCount << ",\n";
        of << "      \"boundary_output_var_count\": " << cluster.m_boundaryOutputVarCount
           << ",\n";
        of << "      \"internal_var_count\": " << cluster.m_internalVarCount << ",\n";
        of << "      \"spec_frontier_candidate_count\": " << cluster.m_specFrontierCandidateCount
           << ",\n";
        of << "      \"spec_frontier_preferred_zero_count\": "
           << cluster.m_specFrontierPreferredZeroCount << ",\n";
        of << "      \"spec_frontier_preferred_one_count\": "
           << cluster.m_specFrontierPreferredOneCount << ",\n";
        of << "      \"spec_frontier_unknown_preference_count\": "
           << cluster.m_specFrontierUnknownPreferenceCount << ",\n";
        of << "      \"input_signature_bit_count\": " << cluster.m_inputSignatureBitCount
           << ",\n";
        of << "      \"boundary_input_bit_count\": " << cluster.m_boundaryInputBitCount << ",\n";
        of << "      \"boundary_output_bit_count\": " << cluster.m_boundaryOutputBitCount
           << ",\n";
        of << "      \"internal_bit_count\": " << cluster.m_internalBitCount << ",\n";
        of << "      \"spec_frontier_max_score\": " << cluster.m_specFrontierMaxScore << ",\n";
        of << "      \"activator_input_var_count\": " << cluster.m_activatorInputVarCount
           << ",\n";
        of << "      \"topo_rank\": " << cluster.m_topoRank << ",\n";
        of << "      \"hybrid_owner_hint\": \"" << jsonEscape(cluster.m_hybridOwnerHint)
           << "\",\n";
        of << "      \"dominant_hierarchy\": \"" << jsonEscape(cluster.m_dominantHierarchy)
           << "\",\n";
        of << "      \"unique_hierarchy_count\": " << cluster.m_uniqueHierarchyCount << ",\n";
        of << "      \"assign_idxs\": ";
        writeVarIndexList(of, cluster.m_assignIdxs, "        ");
        of << ",\n";
        of << "      \"boundary_input_var_idxs\": ";
        writeVarIndexList(of, cluster.m_boundaryInputVarIdxs, "        ");
        of << ",\n";
        of << "      \"boundary_output_var_idxs\": ";
        writeVarIndexList(of, cluster.m_boundaryOutputVarIdxs, "        ");
        of << ",\n";
        of << "      \"internal_var_idxs\": ";
        writeVarIndexList(of, cluster.m_internalVarIdxs, "        ");
        of << ",\n";
        of << "      \"dependency_cluster_idxs\": ";
        writeVarIndexList(of, cluster.m_dependencyClusterIdxs, "        ");
        of << ",\n";
        of << "      \"dependent_cluster_idxs\": ";
        writeVarIndexList(of, cluster.m_dependentClusterIdxs, "        ");
        of << ",\n";
        of << "      \"cpu_boundary_input_var_idxs\": ";
        writeVarIndexList(of, cluster.m_cpuBoundaryInputVarIdxs, "        ");
        of << ",\n";
        of << "      \"gpu_internal_input_var_idxs\": ";
        writeVarIndexList(of, cluster.m_gpuInternalInputVarIdxs, "        ");
        of << ",\n";
        of << "      \"cpu_boundary_output_var_idxs\": ";
        writeVarIndexList(of, cluster.m_cpuBoundaryOutputVarIdxs, "        ");
        of << ",\n";
        of << "      \"gpu_internal_output_var_idxs\": ";
        writeVarIndexList(of, cluster.m_gpuInternalOutputVarIdxs, "        ");
        of << ",\n";
        of << "      \"expr_kind_counts\": ";
        writeExprKindCounts(of, cluster.m_exprKindCounts, "        ", "      ");
        of << ",\n";
        of << "      \"spec_frontier_candidates\": [";
        if (!cluster.m_specFrontierCandidates.empty()) of << "\n";
        for (size_t j = 0; j < cluster.m_specFrontierCandidates.size(); ++j) {
            const auto& candidate = cluster.m_specFrontierCandidates.at(j);
            const V3SimAccelProgram::Var& var = program.m_vars.at(candidate.m_varIdx);
            of << "        {\n";
            of << "          \"var_idx\": " << candidate.m_varIdx << ",\n";
            of << "          \"var_name\": \"" << jsonEscape(var.m_name) << "\",\n";
            of << "          \"width\": " << var.m_width << ",\n";
            of << "          \"assign_use_count\": " << candidate.m_assignUseCount << ",\n";
            of << "          \"zero_bias_count\": " << candidate.m_zeroBiasCount << ",\n";
            of << "          \"one_bias_count\": " << candidate.m_oneBiasCount << ",\n";
            of << "          \"is_activator\": "
               << (candidate.m_isActivator ? "true" : "false") << ",\n";
            of << "          \"preferred_value\": ";
            writePreferredValue(of, candidate.m_preferredValue);
            of << ",\n";
            of << "          \"spec_score\": " << candidate.m_specScore << "\n";
            of << "        }";
            if (j + 1 != cluster.m_specFrontierCandidates.size()) of << ",";
            of << "\n";
        }
        if (!cluster.m_specFrontierCandidates.empty()) of << "      ";
        of << "]";
        of << "\n";
        of << "    }";
        if (i + 1 != approxRegCut.m_clusters.size()) of << ",";
        of << "\n";
    }
    of << "  ]\n";
    of << "}\n";
}
