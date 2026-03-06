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
    if (value == static_cast<size_t>(-1)) {
        of << "null";
    } else {
        of << value;
    }
}

}  // namespace

void V3SimAccelProgramJson::write(const string& filename, const V3SimAccelProgram& program,
                                  const string& strategy) {
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
        of << "      \"is_activator\": " << (var.m_isActivator ? "true" : "false") << "\n";
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
    of << "  ]\n";
    of << "}\n";
}
