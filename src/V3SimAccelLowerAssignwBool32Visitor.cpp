// DESCRIPTION: Verilator: Sim-Accel assignw-bool32 AST Visitor
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

#include "V3SimAccelLowerAssignwBool32Visitor.h"

#include "V3Ast.h"
#include "V3Global.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

bool simAccelIsInternalVar(const AstVar* varp) {
    return varp->name().compare(0, 3, "__V") == 0;
}

bool simAccelWidthSupported(const AstNode* nodep) {
    return nodep && nodep->widthMin() > 0 && nodep->widthMin() <= 32;
}

uint32_t simAccelMaskValue(int width) {
    if (width <= 0) return 0U;
    if (width >= 32) return 0xffffffffU;
    return static_cast<uint32_t>((1ULL << width) - 1ULL);
}

string simAccelExtractHierarchy(const string& name) {
    size_t lastDot = name.rfind("__DOT__");
    if (lastDot == string::npos) return "";
    string hier = name.substr(0, lastDot);
    size_t pos = 0;
    while ((pos = hier.find("__DOT__", pos)) != string::npos) {
        hier.replace(pos, 7, ".");
        pos += 1;
    }
    return hier;
}

string simAccelExtractBaseName(const string& name) {
    size_t lastDot = name.rfind("__DOT__");
    if (lastDot == string::npos) return name;
    return name.substr(lastDot + 7);
}

bool simAccelIsActivator(const string& name) {
    string lowerStr;
    const string baseName = simAccelExtractBaseName(name);
    for (char c : baseName) {
        lowerStr += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return (lowerStr.find("enable") != string::npos || lowerStr == "en"
            || lowerStr.find("_en") != string::npos
            || lowerStr.find("clk_en") != string::npos
            || lowerStr.find("valid") != string::npos
            || lowerStr.find("ready") != string::npos
            || lowerStr.find("active") != string::npos);
}

class SimAccelAssignwBool32Lowerer final : public VNVisitorConst {
private:
    V3SimAccelProgram m_program;
    std::unordered_map<const AstVar*, size_t> m_varToIndex;
    size_t m_skippedUnsupported = 0;
    size_t m_skippedInternal = 0;
    size_t m_skippedNonVarLhs = 0;
    size_t m_skippedTiming = 0;

    size_t rememberVar(const AstVar* varp) {
        UASSERT_OBJ(varp, v3Global.rootp(), "Unexpected null sim-accel variable");
        const auto it = m_varToIndex.find(varp);
        if (it != m_varToIndex.end()) return it->second;

        const size_t idx = m_program.m_vars.size();
        V3SimAccelProgram::Var var;
        var.m_name = varp->name();
        var.m_hierarchy = simAccelExtractHierarchy(varp->name());
        var.m_direction = varp->direction().ascii();
        var.m_width = varp->widthMin();
        var.m_isPrimaryIo = varp->isPrimaryIO();
        var.m_isActivator = simAccelIsActivator(varp->name());
        m_program.m_vars.push_back(std::move(var));
        m_varToIndex.emplace(varp, idx);
        return idx;
    }

    bool isSupportedExpr(const AstNodeExpr* nodep) const {
        if (!nodep || !simAccelWidthSupported(nodep)) return false;
        if (const AstVarRef* const refp = VN_CAST(nodep, VarRef)) {
            const AstVar* const varp = refp->varp();
            return varp && !simAccelIsInternalVar(varp) && simAccelWidthSupported(varp);
        }
        if (const AstConst* const constp = VN_CAST(nodep, Const)) {
            return simAccelWidthSupported(constp) && !constp->num().isFourState();
        }
        if (const AstAnd* const andp = VN_CAST(nodep, And)) {
            return isSupportedExpr(andp->lhsp()) && isSupportedExpr(andp->rhsp());
        }
        if (const AstOr* const orp = VN_CAST(nodep, Or)) {
            return isSupportedExpr(orp->lhsp()) && isSupportedExpr(orp->rhsp());
        }
        if (const AstXor* const xorp = VN_CAST(nodep, Xor)) {
            return isSupportedExpr(xorp->lhsp()) && isSupportedExpr(xorp->rhsp());
        }
        if (const AstLogAnd* const andp = VN_CAST(nodep, LogAnd)) {
            return isSupportedExpr(andp->lhsp()) && isSupportedExpr(andp->rhsp());
        }
        if (const AstLogOr* const orp = VN_CAST(nodep, LogOr)) {
            return isSupportedExpr(orp->lhsp()) && isSupportedExpr(orp->rhsp());
        }
        if (const AstLogNot* const notp = VN_CAST(nodep, LogNot)) {
            return isSupportedExpr(notp->lhsp());
        }
        if (const AstAdd* const addp = VN_CAST(nodep, Add)) {
            return isSupportedExpr(addp->lhsp()) && isSupportedExpr(addp->rhsp());
        }
        if (const AstSub* const subp = VN_CAST(nodep, Sub)) {
            return isSupportedExpr(subp->lhsp()) && isSupportedExpr(subp->rhsp());
        }
        if (const AstEq* const eqp = VN_CAST(nodep, Eq)) {
            return isSupportedExpr(eqp->lhsp()) && isSupportedExpr(eqp->rhsp());
        }
        if (const AstNeq* const neqp = VN_CAST(nodep, Neq)) {
            return isSupportedExpr(neqp->lhsp()) && isSupportedExpr(neqp->rhsp());
        }
        if (const AstCond* const condp = VN_CAST(nodep, Cond)) {
            return isSupportedExpr(condp->condp()) && isSupportedExpr(condp->thenp())
                   && isSupportedExpr(condp->elsep());
        }
        if (const AstConcat* const concatp = VN_CAST(nodep, Concat)) {
            return isSupportedExpr(concatp->lhsp()) && isSupportedExpr(concatp->rhsp())
                   && concatp->lhsp()->widthMin() > 0 && concatp->rhsp()->widthMin() > 0
                   && concatp->rhsp()->widthMin() < 32;
        }
        if (const AstSel* const selp = VN_CAST(nodep, Sel)) {
            return isSupportedExpr(selp->fromp()) && isSupportedExpr(selp->lsbp())
                   && selp->widthConst() > 0 && selp->widthConst() <= 32;
        }
        if (const AstShiftL* const shlp = VN_CAST(nodep, ShiftL)) {
            return isSupportedExpr(shlp->lhsp()) && isSupportedExpr(shlp->rhsp());
        }
        if (const AstShiftLOvr* const shlp = VN_CAST(nodep, ShiftLOvr)) {
            return isSupportedExpr(shlp->lhsp()) && isSupportedExpr(shlp->rhsp());
        }
        if (const AstShiftR* const shrp = VN_CAST(nodep, ShiftR)) {
            return isSupportedExpr(shrp->lhsp()) && isSupportedExpr(shrp->rhsp());
        }
        if (const AstShiftROvr* const shrp = VN_CAST(nodep, ShiftROvr)) {
            return isSupportedExpr(shrp->lhsp()) && isSupportedExpr(shrp->rhsp());
        }
        if (const AstNot* const notp = VN_CAST(nodep, Not)) return isSupportedExpr(notp->lhsp());
        if (const AstCCast* const castp = VN_CAST(nodep, CCast)) {
            return isSupportedExpr(castp->lhsp());
        }
        if (const AstExtend* const extp = VN_CAST(nodep, Extend)) {
            return isSupportedExpr(extp->lhsp());
        }
        if (const AstExtendS* const extp = VN_CAST(nodep, ExtendS)) {
            return isSupportedExpr(extp->lhsp());
        }
        return false;
    }

    void collectExprVars(const AstNodeExpr* nodep, std::unordered_set<const AstVar*>& currentRhsVars) {
        if (!nodep) return;
        if (const AstVarRef* const refp = VN_CAST(nodep, VarRef)) {
            const AstVar* const varp = refp->varp();
            if (varp && !simAccelIsInternalVar(varp) && simAccelWidthSupported(varp)) {
                rememberVar(varp);
                currentRhsVars.insert(varp);
            }
            return;
        }
        if (const AstAnd* const andp = VN_CAST(nodep, And)) {
            collectExprVars(andp->lhsp(), currentRhsVars);
            collectExprVars(andp->rhsp(), currentRhsVars);
            return;
        }
        if (const AstOr* const orp = VN_CAST(nodep, Or)) {
            collectExprVars(orp->lhsp(), currentRhsVars);
            collectExprVars(orp->rhsp(), currentRhsVars);
            return;
        }
        if (const AstXor* const xorp = VN_CAST(nodep, Xor)) {
            collectExprVars(xorp->lhsp(), currentRhsVars);
            collectExprVars(xorp->rhsp(), currentRhsVars);
            return;
        }
        if (const AstLogAnd* const andp = VN_CAST(nodep, LogAnd)) {
            collectExprVars(andp->lhsp(), currentRhsVars);
            collectExprVars(andp->rhsp(), currentRhsVars);
            return;
        }
        if (const AstLogOr* const orp = VN_CAST(nodep, LogOr)) {
            collectExprVars(orp->lhsp(), currentRhsVars);
            collectExprVars(orp->rhsp(), currentRhsVars);
            return;
        }
        if (const AstLogNot* const notp = VN_CAST(nodep, LogNot)) {
            collectExprVars(notp->lhsp(), currentRhsVars);
            return;
        }
        if (const AstAdd* const addp = VN_CAST(nodep, Add)) {
            collectExprVars(addp->lhsp(), currentRhsVars);
            collectExprVars(addp->rhsp(), currentRhsVars);
            return;
        }
        if (const AstSub* const subp = VN_CAST(nodep, Sub)) {
            collectExprVars(subp->lhsp(), currentRhsVars);
            collectExprVars(subp->rhsp(), currentRhsVars);
            return;
        }
        if (const AstEq* const eqp = VN_CAST(nodep, Eq)) {
            collectExprVars(eqp->lhsp(), currentRhsVars);
            collectExprVars(eqp->rhsp(), currentRhsVars);
            return;
        }
        if (const AstNeq* const neqp = VN_CAST(nodep, Neq)) {
            collectExprVars(neqp->lhsp(), currentRhsVars);
            collectExprVars(neqp->rhsp(), currentRhsVars);
            return;
        }
        if (const AstCond* const condp = VN_CAST(nodep, Cond)) {
            collectExprVars(condp->condp(), currentRhsVars);
            collectExprVars(condp->thenp(), currentRhsVars);
            collectExprVars(condp->elsep(), currentRhsVars);
            return;
        }
        if (const AstConcat* const concatp = VN_CAST(nodep, Concat)) {
            collectExprVars(concatp->lhsp(), currentRhsVars);
            collectExprVars(concatp->rhsp(), currentRhsVars);
            return;
        }
        if (const AstSel* const selp = VN_CAST(nodep, Sel)) {
            collectExprVars(selp->fromp(), currentRhsVars);
            collectExprVars(selp->lsbp(), currentRhsVars);
            return;
        }
        if (const AstShiftL* const shlp = VN_CAST(nodep, ShiftL)) {
            collectExprVars(shlp->lhsp(), currentRhsVars);
            collectExprVars(shlp->rhsp(), currentRhsVars);
            return;
        }
        if (const AstShiftLOvr* const shlp = VN_CAST(nodep, ShiftLOvr)) {
            collectExprVars(shlp->lhsp(), currentRhsVars);
            collectExprVars(shlp->rhsp(), currentRhsVars);
            return;
        }
        if (const AstShiftR* const shrp = VN_CAST(nodep, ShiftR)) {
            collectExprVars(shrp->lhsp(), currentRhsVars);
            collectExprVars(shrp->rhsp(), currentRhsVars);
            return;
        }
        if (const AstShiftROvr* const shrp = VN_CAST(nodep, ShiftROvr)) {
            collectExprVars(shrp->lhsp(), currentRhsVars);
            collectExprVars(shrp->rhsp(), currentRhsVars);
            return;
        }
        if (const AstNot* const notp = VN_CAST(nodep, Not)) {
            collectExprVars(notp->lhsp(), currentRhsVars);
            return;
        }
        if (const AstCCast* const castp = VN_CAST(nodep, CCast)) {
            collectExprVars(castp->lhsp(), currentRhsVars);
            return;
        }
        if (const AstExtend* const extp = VN_CAST(nodep, Extend)) {
            collectExprVars(extp->lhsp(), currentRhsVars);
            return;
        }
        if (const AstExtendS* const extp = VN_CAST(nodep, ExtendS)) {
            collectExprVars(extp->lhsp(), currentRhsVars);
            return;
        }
    }

    size_t addExpr(V3SimAccelProgram::Expr expr) {
        const size_t idx = m_program.m_exprs.size();
        m_program.m_exprs.push_back(std::move(expr));
        return idx;
    }

    size_t lowerExpr(const AstNodeExpr* nodep) {
        UASSERT_OBJ(nodep, v3Global.rootp(), "Unexpected null expression");

        V3SimAccelProgram::Expr expr;
        expr.m_width = nodep->widthMin();
        if (const AstVarRef* const refp = VN_CAST(nodep, VarRef)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::VAR;
            expr.m_varIdx = rememberVar(refp->varp());
            return addExpr(std::move(expr));
        }
        if (const AstConst* const constp = VN_CAST(nodep, Const)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::CONST;
            expr.m_constValue = constp->toUInt() & simAccelMaskValue(constp->widthMin());
            return addExpr(std::move(expr));
        }
        if (const AstAnd* const andp = VN_CAST(nodep, And)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::AND;
            expr.m_lhs = lowerExpr(andp->lhsp());
            expr.m_rhs = lowerExpr(andp->rhsp());
            return addExpr(std::move(expr));
        }
        if (const AstOr* const orp = VN_CAST(nodep, Or)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::OR;
            expr.m_lhs = lowerExpr(orp->lhsp());
            expr.m_rhs = lowerExpr(orp->rhsp());
            return addExpr(std::move(expr));
        }
        if (const AstXor* const xorp = VN_CAST(nodep, Xor)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::XOR;
            expr.m_lhs = lowerExpr(xorp->lhsp());
            expr.m_rhs = lowerExpr(xorp->rhsp());
            return addExpr(std::move(expr));
        }
        if (const AstLogAnd* const andp = VN_CAST(nodep, LogAnd)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::LOGAND;
            expr.m_lhs = lowerExpr(andp->lhsp());
            expr.m_rhs = lowerExpr(andp->rhsp());
            return addExpr(std::move(expr));
        }
        if (const AstLogOr* const orp = VN_CAST(nodep, LogOr)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::LOGOR;
            expr.m_lhs = lowerExpr(orp->lhsp());
            expr.m_rhs = lowerExpr(orp->rhsp());
            return addExpr(std::move(expr));
        }
        if (const AstLogNot* const notp = VN_CAST(nodep, LogNot)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::LOGNOT;
            expr.m_lhs = lowerExpr(notp->lhsp());
            return addExpr(std::move(expr));
        }
        if (const AstAdd* const addp = VN_CAST(nodep, Add)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::ADD;
            expr.m_lhs = lowerExpr(addp->lhsp());
            expr.m_rhs = lowerExpr(addp->rhsp());
            return addExpr(std::move(expr));
        }
        if (const AstSub* const subp = VN_CAST(nodep, Sub)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::SUB;
            expr.m_lhs = lowerExpr(subp->lhsp());
            expr.m_rhs = lowerExpr(subp->rhsp());
            return addExpr(std::move(expr));
        }
        if (const AstEq* const eqp = VN_CAST(nodep, Eq)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::EQ;
            expr.m_lhs = lowerExpr(eqp->lhsp());
            expr.m_rhs = lowerExpr(eqp->rhsp());
            expr.m_width = 1;
            return addExpr(std::move(expr));
        }
        if (const AstNeq* const neqp = VN_CAST(nodep, Neq)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::NEQ;
            expr.m_lhs = lowerExpr(neqp->lhsp());
            expr.m_rhs = lowerExpr(neqp->rhsp());
            expr.m_width = 1;
            return addExpr(std::move(expr));
        }
        if (const AstCond* const condp = VN_CAST(nodep, Cond)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::COND;
            expr.m_lhs = lowerExpr(condp->condp());
            expr.m_rhs = lowerExpr(condp->thenp());
            expr.m_third = lowerExpr(condp->elsep());
            return addExpr(std::move(expr));
        }
        if (const AstConcat* const concatp = VN_CAST(nodep, Concat)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::CONCAT;
            expr.m_lhs = lowerExpr(concatp->lhsp());
            expr.m_rhs = lowerExpr(concatp->rhsp());
            return addExpr(std::move(expr));
        }
        if (const AstSel* const selp = VN_CAST(nodep, Sel)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::SEL;
            expr.m_lhs = lowerExpr(selp->fromp());
            expr.m_rhs = lowerExpr(selp->lsbp());
            expr.m_width = selp->widthConst();
            return addExpr(std::move(expr));
        }
        if (const AstShiftL* const shlp = VN_CAST(nodep, ShiftL)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::SHIFTL;
            expr.m_lhs = lowerExpr(shlp->lhsp());
            expr.m_rhs = lowerExpr(shlp->rhsp());
            return addExpr(std::move(expr));
        }
        if (const AstShiftLOvr* const shlp = VN_CAST(nodep, ShiftLOvr)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::SHIFTLOVR;
            expr.m_lhs = lowerExpr(shlp->lhsp());
            expr.m_rhs = lowerExpr(shlp->rhsp());
            return addExpr(std::move(expr));
        }
        if (const AstShiftR* const shrp = VN_CAST(nodep, ShiftR)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::SHIFTR;
            expr.m_lhs = lowerExpr(shrp->lhsp());
            expr.m_rhs = lowerExpr(shrp->rhsp());
            return addExpr(std::move(expr));
        }
        if (const AstShiftROvr* const shrp = VN_CAST(nodep, ShiftROvr)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::SHIFTROVR;
            expr.m_lhs = lowerExpr(shrp->lhsp());
            expr.m_rhs = lowerExpr(shrp->rhsp());
            return addExpr(std::move(expr));
        }
        if (const AstNot* const notp = VN_CAST(nodep, Not)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::NOT;
            expr.m_lhs = lowerExpr(notp->lhsp());
            return addExpr(std::move(expr));
        }
        if (const AstCCast* const castp = VN_CAST(nodep, CCast)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::CCAST;
            expr.m_lhs = lowerExpr(castp->lhsp());
            return addExpr(std::move(expr));
        }
        if (const AstExtend* const extp = VN_CAST(nodep, Extend)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::EXTEND;
            expr.m_lhs = lowerExpr(extp->lhsp());
            return addExpr(std::move(expr));
        }
        if (const AstExtendS* const extp = VN_CAST(nodep, ExtendS)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::EXTENDS;
            expr.m_lhs = lowerExpr(extp->lhsp());
            return addExpr(std::move(expr));
        }

        nodep->v3fatalSrc("Unsupported node for --sim-accel-only: " << nodep->typeName());
        VL_UNREACHABLE;
    }

public:
    explicit SimAccelAssignwBool32Lowerer(AstNetlist* rootp) { iterateConst(rootp); }

    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

    void visit(AstAssignW* nodep) override {
        if (nodep->timingControlp()) {
            ++m_skippedTiming;
            return;
        }
        AstVarRef* const lhsRefp = VN_CAST(nodep->lhsp(), VarRef);
        if (!lhsRefp) {
            ++m_skippedNonVarLhs;
            return;
        }
        AstVar* const lhsVarp = lhsRefp->varp();
        if (!lhsVarp || simAccelIsInternalVar(lhsVarp)) {
            ++m_skippedInternal;
            return;
        }
        if (!simAccelWidthSupported(lhsVarp) || !isSupportedExpr(nodep->rhsp())) {
            ++m_skippedUnsupported;
            return;
        }

        const size_t lhsIdx = rememberVar(lhsVarp);
        std::unordered_set<const AstVar*> rhsVars;
        collectExprVars(nodep->rhsp(), rhsVars);
        std::vector<const AstVar*> rhsVarsVec{rhsVars.begin(), rhsVars.end()};
        std::sort(rhsVarsVec.begin(), rhsVarsVec.end(),
                  [this](const AstVar* a, const AstVar* b) {
                      const auto ita = m_varToIndex.find(a);
                      const auto itb = m_varToIndex.find(b);
                      if (ita != m_varToIndex.end() && itb != m_varToIndex.end()) {
                          return ita->second < itb->second;
                      }
                      return a->name() < b->name();
                  });

        V3SimAccelProgram::Assign assign;
        assign.m_lhsIdx = lhsIdx;
        assign.m_lhsName = lhsVarp->name();
        assign.m_exprIdx = lowerExpr(nodep->rhsp());
        assign.m_rhsIdxs.reserve(rhsVarsVec.size());
        for (const AstVar* const rhsVarp : rhsVarsVec) {
            const auto it = m_varToIndex.find(rhsVarp);
            if (it != m_varToIndex.end()) assign.m_rhsIdxs.push_back(it->second);
        }
        m_program.m_assigns.push_back(std::move(assign));
        ++m_program.m_stats.m_assignwSupported;
    }

    V3SimAccelProgram program() && {
        m_program.m_stats.m_assignwIgnored = m_skippedUnsupported + m_skippedInternal
                                             + m_skippedNonVarLhs + m_skippedTiming;
        m_program.m_stats.m_assignwTotal = m_program.m_stats.m_assignwSupported
                                           + m_program.m_stats.m_assignwIgnored;
        return std::move(m_program);
    }
};

}  // namespace

V3SimAccelProgram V3SimAccelLowerAssignwBool32Visitor::build(AstNetlist* rootp) {
    SimAccelAssignwBool32Lowerer lowerer{rootp};
    return std::move(lowerer).program();
}
