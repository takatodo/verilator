// DESCRIPTION: Verilator: Sim-Accel CUDA Expr Emitter
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

#include "V3SimAccelBackendCudaExprEmitter.h"

#include "V3Error.h"
#include "V3Global.h"

#include <sstream>

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

}  // namespace

V3SimAccelBackendCudaExprEmitter::V3SimAccelBackendCudaExprEmitter(
    const V3SimAccelProgram& program)
    : m_program{program} {}

string V3SimAccelBackendCudaExprEmitter::maskWrap(const string& expr, uint32_t width) const {
    if (width >= 32) return "(" + expr + ")";
    return "((" + expr + ") & " + simAccelMaskLiteral(width) + ")";
}

string V3SimAccelBackendCudaExprEmitter::emit(size_t exprIdx) const {
    UASSERT_OBJ(exprIdx < m_program.m_exprs.size(), v3Global.rootp(),
                "Invalid sim-accel expression index");
    const V3SimAccelProgram::Expr& expr = m_program.m_exprs.at(exprIdx);
    switch (expr.m_kind) {
    case V3SimAccelProgram::ExprKind::VAR:
        return "v_" + cvtToStr(expr.m_varIdx);
    case V3SimAccelProgram::ExprKind::CONST: {
        std::ostringstream os;
        os << "0x" << std::hex << std::nouppercase << expr.m_constValue << "u";
        return maskWrap(os.str(), expr.m_width);
    }
    case V3SimAccelProgram::ExprKind::AND:
        return maskWrap(emit(expr.m_lhs) + " & " + emit(expr.m_rhs), expr.m_width);
    case V3SimAccelProgram::ExprKind::OR:
        return maskWrap(emit(expr.m_lhs) + " | " + emit(expr.m_rhs), expr.m_width);
    case V3SimAccelProgram::ExprKind::XOR:
        return maskWrap(emit(expr.m_lhs) + " ^ " + emit(expr.m_rhs), expr.m_width);
    case V3SimAccelProgram::ExprKind::LOGAND:
        return "(((" + emit(expr.m_lhs) + ") != 0u && (" + emit(expr.m_rhs)
               + ") != 0u) ? 1u : 0u)";
    case V3SimAccelProgram::ExprKind::LOGOR:
        return "(((" + emit(expr.m_lhs) + ") != 0u || (" + emit(expr.m_rhs)
               + ") != 0u) ? 1u : 0u)";
    case V3SimAccelProgram::ExprKind::LOGNOT:
        return "(((" + emit(expr.m_lhs) + ") == 0u) ? 1u : 0u)";
    case V3SimAccelProgram::ExprKind::ADD:
        return maskWrap(emit(expr.m_lhs) + " + " + emit(expr.m_rhs), expr.m_width);
    case V3SimAccelProgram::ExprKind::SUB:
        return maskWrap(emit(expr.m_lhs) + " - " + emit(expr.m_rhs), expr.m_width);
    case V3SimAccelProgram::ExprKind::EQ:
        return "(((" + emit(expr.m_lhs) + ") == (" + emit(expr.m_rhs) + ")) ? 1u : 0u)";
    case V3SimAccelProgram::ExprKind::NEQ:
        return "(((" + emit(expr.m_lhs) + ") != (" + emit(expr.m_rhs) + ")) ? 1u : 0u)";
    case V3SimAccelProgram::ExprKind::COND:
        return maskWrap("((" + emit(expr.m_lhs) + ") ? (" + emit(expr.m_rhs) + ") : ("
                            + emit(expr.m_third) + "))",
                        expr.m_width);
    case V3SimAccelProgram::ExprKind::CONCAT: {
        const uint32_t rhsWidth = m_program.m_exprs.at(expr.m_rhs).m_width;
        return maskWrap("((" + emit(expr.m_lhs) + " << " + cvtToStr(rhsWidth) + ") | (("
                            + emit(expr.m_rhs) + ") & " + simAccelMaskLiteral(rhsWidth) + "))",
                        expr.m_width);
    }
    case V3SimAccelProgram::ExprKind::SEL:
        return maskWrap("((" + emit(expr.m_lhs) + " >> ((" + emit(expr.m_rhs)
                            + ") & 31u)) & " + simAccelMaskLiteral(expr.m_width) + ")",
                        expr.m_width);
    case V3SimAccelProgram::ExprKind::SHIFTL:
    case V3SimAccelProgram::ExprKind::SHIFTLOVR: {
        const string sh = emit(expr.m_rhs);
        return maskWrap("((" + sh + " >= 32u) ? 0u : (" + emit(expr.m_lhs) + " << (("
                            + sh + ") & 31u)))",
                        expr.m_width);
    }
    case V3SimAccelProgram::ExprKind::SHIFTR:
    case V3SimAccelProgram::ExprKind::SHIFTROVR: {
        const string sh = emit(expr.m_rhs);
        return maskWrap("((" + sh + " >= 32u) ? 0u : (" + emit(expr.m_lhs) + " >> (("
                            + sh + ") & 31u)))",
                        expr.m_width);
    }
    case V3SimAccelProgram::ExprKind::NOT:
        return maskWrap("~" + emit(expr.m_lhs), expr.m_width);
    case V3SimAccelProgram::ExprKind::CCAST:
    case V3SimAccelProgram::ExprKind::EXTEND:
        return maskWrap(emit(expr.m_lhs), expr.m_width);
    case V3SimAccelProgram::ExprKind::EXTENDS: {
        const V3SimAccelProgram::Expr& child = m_program.m_exprs.at(expr.m_lhs);
        if (child.m_width == 0 || child.m_width >= expr.m_width || child.m_width >= 32) {
            return maskWrap(emit(expr.m_lhs), expr.m_width);
        }
        const string childExpr = emit(expr.m_lhs);
        const string childMask = simAccelMaskLiteral(child.m_width);
        const string sign = "(((" + childExpr + ") >> " + cvtToStr(child.m_width - 1)
                            + ") & 1u)";
        return maskWrap("((" + sign + " != 0u) ? ((" + childExpr + ") | (~" + childMask
                            + ")) : (" + childExpr + "))",
                        expr.m_width);
    }
    }
    v3fatal("Unsupported sim-accel expression kind");  // LCOV_EXCL_LINE
}
