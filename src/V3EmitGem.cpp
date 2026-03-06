// DESCRIPTION: Verilator: GEM Back-end Emitter
//
// This file is responsible for emitting GEM (GPU-Accelerated Emulator-Inspired
// RTL Simulation) intermediate representation and CUDA kernels.
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

#include "V3EmitGem.h"

#include "V3Ast.h"
#include "V3Error.h"
#include "V3Global.h"
#include "V3Options.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

void V3EmitGem::emitGemIr() VL_MT_DISABLED {
    const string filename = (v3Global.opt.gemIrOutput().empty()
                                 ? v3Global.opt.makeDir() + "/" + v3Global.opt.prefix()
                                       + ".sim_accel.tree.json"
                                 : v3Global.opt.gemIrOutput());
    v3Global.rootp()->dumpTreeJsonFile(filename);
}

namespace {

bool gemCudaIsInternalVar(const AstVar* varp) {
    return varp->name().compare(0, 3, "__V") == 0;
}

bool gemCudaWidthSupported(const AstNode* nodep) {
    return nodep && nodep->widthMin() > 0 && nodep->widthMin() <= 32;
}

uint32_t gemCudaMaskValue(int width) {
    if (width <= 0) return 0U;
    if (width >= 32) return 0xffffffffU;
    return static_cast<uint32_t>((1ULL << width) - 1ULL);
}

string gemCudaMaskLiteral(int width) {
    std::ostringstream os;
    os << "0x" << std::hex << std::nouppercase << gemCudaMaskValue(width) << "u";
    return os.str();
}

string gemCudaEscapeCString(const string& in) {
    string out;
    out.reserve(in.size() + 8);
    for (char ch : in) {
        if (ch == '\\' || ch == '"') out += '\\';
        out += ch;
    }
    return out;
}

string gemExtractHierarchy(const string& name) {
    size_t lastDot = name.rfind("__DOT__");
    if (lastDot == string::npos) return "";  // Directly under top module
    string hier = name.substr(0, lastDot);
    // Replace __DOT__ with '.' for hierarchy path readability.
    size_t pos = 0;
    while ((pos = hier.find("__DOT__", pos)) != string::npos) {
        hier.replace(pos, 7, ".");
        pos += 1;
    }
    return hier;
}

string gemExtractBaseName(const string& name) {
    size_t lastDot = name.rfind("__DOT__");
    if (lastDot == string::npos) return name;
    return name.substr(lastDot + 7);
}

bool gemIsActivator(const string& name) {
    string lowerStr;
    string baseName = gemExtractBaseName(name);
    for (char c : baseName) lowerStr += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return (lowerStr.find("enable") != string::npos ||
            lowerStr == "en" || lowerStr.find("_en") != string::npos ||
            lowerStr.find("clk_en") != string::npos ||
            lowerStr.find("valid") != string::npos ||
            lowerStr.find("ready") != string::npos ||
            lowerStr.find("active") != string::npos);
}

class GemCudaCollector final : public VNVisitorConst {
public:
    struct AssignRec final {
        AstVar* m_lhsVarp = nullptr;
        const AstNodeExpr* m_rhsp = nullptr;
        std::vector<const AstVar*> m_rhsVarps;
    };

private:
    std::vector<AssignRec> m_assigns;
    std::vector<AstVar*> m_vars;
    std::unordered_map<const AstVar*, size_t> m_varToIndex;
    size_t m_skippedUnsupported = 0;
    size_t m_skippedInternal = 0;
    size_t m_skippedNonVarLhs = 0;
    size_t m_skippedTiming = 0;

    bool rememberVar(const AstVar* varp) {
        if (!varp || gemCudaIsInternalVar(varp) || !gemCudaWidthSupported(varp)) return false;
        if (m_varToIndex.find(varp) == m_varToIndex.end()) {
            m_varToIndex.emplace(varp, m_vars.size());
            m_vars.push_back(const_cast<AstVar*>(varp));
        }
        return true;
    }

    bool isSupportedExpr(const AstNodeExpr* nodep) const {
        if (!nodep || !gemCudaWidthSupported(nodep)) return false;
        if (const AstVarRef* const refp = VN_CAST(nodep, VarRef)) {
            const AstVar* const varp = refp->varp();
            return varp && !gemCudaIsInternalVar(varp) && gemCudaWidthSupported(varp);
        }
        if (const AstConst* const constp = VN_CAST(nodep, Const)) {
            return gemCudaWidthSupported(constp) && !constp->num().isFourState();
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
            rememberVar(refp->varp());
            const AstVar* varp = refp->varp();
            if (varp && !gemCudaIsInternalVar(varp) && gemCudaWidthSupported(varp)) {
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

public:
    explicit GemCudaCollector(AstNetlist* rootp) { iterateConst(rootp); }

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
        if (!lhsVarp || gemCudaIsInternalVar(lhsVarp)) {
            ++m_skippedInternal;
            return;
        }
        if (!rememberVar(lhsVarp) || !isSupportedExpr(nodep->rhsp())) {
            ++m_skippedUnsupported;
            return;
        }
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
        m_assigns.push_back({lhsVarp, nodep->rhsp(), std::move(rhsVarsVec)});
    }

    const std::vector<AssignRec>& assigns() const { return m_assigns; }
    const std::vector<AstVar*>& vars() const { return m_vars; }
    const std::unordered_map<const AstVar*, size_t>& varToIndex() const { return m_varToIndex; }
    size_t skippedUnsupported() const { return m_skippedUnsupported; }
    size_t skippedInternal() const { return m_skippedInternal; }
    size_t skippedNonVarLhs() const { return m_skippedNonVarLhs; }
    size_t skippedTiming() const { return m_skippedTiming; }
};

class GemCudaExprEmitter final {
    const std::unordered_map<const AstVar*, size_t>& m_varToIndex;

    string maskWrap(const string& expr, int width) const {
        if (width >= 32) return "(" + expr + ")";
        return "((" + expr + ") & " + gemCudaMaskLiteral(width) + ")";
    }

public:
    explicit GemCudaExprEmitter(const std::unordered_map<const AstVar*, size_t>& varToIndex)
        : m_varToIndex{varToIndex} {}

    string emit(const AstNodeExpr* nodep) const {
        UASSERT_OBJ(nodep, v3Global.rootp(), "Unexpected null expression");
        if (const AstVarRef* const refp = VN_CAST(nodep, VarRef)) {
            const size_t idx = m_varToIndex.at(refp->varp());
            return "v_" + cvtToStr(idx);
        }
        if (const AstConst* const constp = VN_CAST(nodep, Const)) {
            const uint32_t val = constp->toUInt() & gemCudaMaskValue(constp->widthMin());
            std::ostringstream os;
            os << "0x" << std::hex << std::nouppercase << val << "u";
            return maskWrap(os.str(), constp->widthMin());
        }
        if (const AstAnd* const andp = VN_CAST(nodep, And)) {
            return maskWrap(emit(andp->lhsp()) + " & " + emit(andp->rhsp()), nodep->widthMin());
        }
        if (const AstOr* const orp = VN_CAST(nodep, Or)) {
            return maskWrap(emit(orp->lhsp()) + " | " + emit(orp->rhsp()), nodep->widthMin());
        }
        if (const AstXor* const xorp = VN_CAST(nodep, Xor)) {
            return maskWrap(emit(xorp->lhsp()) + " ^ " + emit(xorp->rhsp()), nodep->widthMin());
        }
        if (const AstLogAnd* const andp = VN_CAST(nodep, LogAnd)) {
            return "(((" + emit(andp->lhsp()) + ") != 0u && (" + emit(andp->rhsp())
                   + ") != 0u) ? 1u : 0u)";
        }
        if (const AstLogOr* const orp = VN_CAST(nodep, LogOr)) {
            return "(((" + emit(orp->lhsp()) + ") != 0u || (" + emit(orp->rhsp())
                   + ") != 0u) ? 1u : 0u)";
        }
        if (const AstLogNot* const notp = VN_CAST(nodep, LogNot)) {
            return "(((" + emit(notp->lhsp()) + ") == 0u) ? 1u : 0u)";
        }
        if (const AstAdd* const addp = VN_CAST(nodep, Add)) {
            return maskWrap(emit(addp->lhsp()) + " + " + emit(addp->rhsp()), nodep->widthMin());
        }
        if (const AstSub* const subp = VN_CAST(nodep, Sub)) {
            return maskWrap(emit(subp->lhsp()) + " - " + emit(subp->rhsp()), nodep->widthMin());
        }
        if (const AstEq* const eqp = VN_CAST(nodep, Eq)) {
            return "(((" + emit(eqp->lhsp()) + ") == (" + emit(eqp->rhsp()) + ")) ? 1u : 0u)";
        }
        if (const AstNeq* const neqp = VN_CAST(nodep, Neq)) {
            return "(((" + emit(neqp->lhsp()) + ") != (" + emit(neqp->rhsp()) + ")) ? 1u : 0u)";
        }
        if (const AstCond* const condp = VN_CAST(nodep, Cond)) {
            return maskWrap("((" + emit(condp->condp()) + ") ? (" + emit(condp->thenp()) + ") : ("
                                + emit(condp->elsep()) + "))",
                            nodep->widthMin());
        }
        if (const AstConcat* const concatp = VN_CAST(nodep, Concat)) {
            const int rhsWidth = concatp->rhsp()->widthMin();
            const string rhsMask = gemCudaMaskLiteral(rhsWidth);
            return maskWrap("((" + emit(concatp->lhsp()) + " << " + cvtToStr(rhsWidth) + ") | (("
                                + emit(concatp->rhsp()) + ") & " + rhsMask + "))",
                            nodep->widthMin());
        }
        if (const AstSel* const selp = VN_CAST(nodep, Sel)) {
            const string wmask = gemCudaMaskLiteral(selp->widthConst());
            const string lsb = emit(selp->lsbp());
            return maskWrap("((" + emit(selp->fromp()) + " >> (" + lsb + " & 31u)) & " + wmask + ")",
                            nodep->widthMin());
        }
        if (const AstShiftL* const shlp = VN_CAST(nodep, ShiftL)) {
            const string sh = emit(shlp->rhsp());
            return maskWrap("((" + sh + " >= 32u) ? 0u : (" + emit(shlp->lhsp()) + " << (" + sh + " & 31u)))",
                            nodep->widthMin());
        }
        if (const AstShiftLOvr* const shlp = VN_CAST(nodep, ShiftLOvr)) {
            const string sh = emit(shlp->rhsp());
            return maskWrap("((" + sh + " >= 32u) ? 0u : (" + emit(shlp->lhsp()) + " << (" + sh + " & 31u)))",
                            nodep->widthMin());
        }
        if (const AstShiftR* const shrp = VN_CAST(nodep, ShiftR)) {
            const string sh = emit(shrp->rhsp());
            return maskWrap("((" + sh + " >= 32u) ? 0u : (" + emit(shrp->lhsp()) + " >> (" + sh + " & 31u)))",
                            nodep->widthMin());
        }
        if (const AstShiftROvr* const shrp = VN_CAST(nodep, ShiftROvr)) {
            const string sh = emit(shrp->rhsp());
            return maskWrap("((" + sh + " >= 32u) ? 0u : (" + emit(shrp->lhsp()) + " >> (" + sh + " & 31u)))",
                            nodep->widthMin());
        }
        if (const AstNot* const notp = VN_CAST(nodep, Not)) {
            return maskWrap("~" + emit(notp->lhsp()), nodep->widthMin());
        }
        if (const AstCCast* const castp = VN_CAST(nodep, CCast)) {
            return maskWrap(emit(castp->lhsp()), nodep->widthMin());
        }
        if (const AstExtend* const extp = VN_CAST(nodep, Extend)) {
            return maskWrap(emit(extp->lhsp()), nodep->widthMin());
        }
        if (const AstExtendS* const extp = VN_CAST(nodep, ExtendS)) {
            return maskWrap(emit(extp->lhsp()), nodep->widthMin());
        }
        nodep->v3fatalSrc("Unsupported node for --sim-accel-only: " << nodep->typeName());
        VL_UNREACHABLE;
    }
};

}  // namespace

void V3EmitGem::emitGemCuda() VL_MT_DISABLED {
    const string filename = (v3Global.opt.gemCudaOutput().empty()
                                 ? v3Global.opt.makeDir() + "/" + v3Global.opt.prefix()
                                       + ".sim_accel.kernel.cu"
                                 : v3Global.opt.gemCudaOutput());
    if (v3Global.opt.simAccelSplitModules()) {
        v3fatal("--sim-accel-split-modules is not implemented yet");  // LCOV_EXCL_LINE
    }

    const GemCudaCollector collector{v3Global.rootp()};
    if (collector.assigns().empty()) {
        v3fatal("No supported ASSIGNW nodes found for --sim-accel-only "  // LCOV_EXCL_LINE
                "(supported expr ops: VARREF/CONST/AND/OR/XOR/LOGAND/LOGOR/LOGNOT/NOT/ADD/SUB/"
                "EQ/NEQ/COND/CONCAT/SEL/SHIFT plus CCAST/EXTEND wrappers)");
    }

    const size_t supportedAssignw = collector.assigns().size();
    const size_t skipped = collector.skippedUnsupported() + collector.skippedInternal()
                           + collector.skippedNonVarLhs() + collector.skippedTiming();
    const size_t totalAssignw = supportedAssignw + skipped;
    if (skipped) {
        v3info("--sim-accel-only ignored " << skipped
                                          << " ASSIGNW nodes (unsupported or internal "
                                             "constructs)");
    }

    std::ofstream of{filename};
    if (!of.is_open()) v3fatal("Cannot open output file: " + filename);  // LCOV_EXCL_LINE

    of << "// Generated by Verilator --sim-accel-only\n"
       << "// Supported node subset: ASSIGNW + bool/arith/select/shift/concat (+ casts/extends)\n"
       << "#include <stdint.h>\n\n";
    of << "#define gem_eval_assignw_u32 sim_accel_eval_assignw_u32\n"
       << "#define gem_eval_var_count sim_accel_eval_var_count\n"
       << "#define gem_eval_var_name sim_accel_eval_var_name\n\n";

    of << "extern \"C\" __global__ void sim_accel_eval_assignw_u32(const uint32_t* state_in,\n"
       << "                                                  uint32_t* state_out,\n"
       << "                                                  uint32_t nstates) {\n"
       << "    const uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;\n"
       << "    if (tid >= nstates) return;\n";

    struct EmittedAssign final {
        size_t m_lhsIdx = 0;
        const AstVar* m_lhsVarp = nullptr;
        string m_expr;
        std::vector<size_t> m_rhsIdxs;
    };
    const auto& varToIndex = collector.varToIndex();
    const GemCudaExprEmitter emitter{varToIndex};
    std::unordered_set<string> emittedAssignKeys;
    std::vector<EmittedAssign> emittedAssigns;
    std::unordered_set<size_t> writtenVarIdxs;
    std::unordered_set<size_t> readVarIdxs;

    for (const GemCudaCollector::AssignRec& as : collector.assigns()) {
        const size_t lhsIdx = varToIndex.at(as.m_lhsVarp);
        const string expr = emitter.emit(as.m_rhsp);
        if (expr == ("v_" + cvtToStr(lhsIdx))) continue;  // Skip no-op self-assign
        const string key = cvtToStr(lhsIdx) + "|" + expr;
        if (!emittedAssignKeys.emplace(key).second) continue;

        EmittedAssign rec;
        rec.m_lhsIdx = lhsIdx;
        rec.m_lhsVarp = as.m_lhsVarp;
        rec.m_expr = expr;
        rec.m_rhsIdxs.reserve(as.m_rhsVarps.size());
        for (const AstVar* const rhsVar : as.m_rhsVarps) {
            const auto it = varToIndex.find(rhsVar);
            if (it == varToIndex.end()) continue;
            rec.m_rhsIdxs.push_back(it->second);
            readVarIdxs.emplace(it->second);
        }
        emittedAssigns.push_back(std::move(rec));
        writtenVarIdxs.emplace(lhsIdx);
    }

    const size_t emittedUniqueAssignw = emittedAssigns.size();

    for (size_t i = 0; i < collector.vars().size(); ++i) {
        const AstVar* const varp = collector.vars().at(i);
        if (readVarIdxs.find(i) != readVarIdxs.end()) {
            of << "    uint32_t v_" << i << " = state_in[" << i << "U * nstates + tid] & "
               << gemCudaMaskLiteral(varp->widthMin()) << ";  // " << varp->name() << "\n";
        } else if (writtenVarIdxs.find(i) != writtenVarIdxs.end()) {
            of << "    uint32_t v_" << i << " = 0u;  // " << varp->name()
               << " (write-only)\n";
        }
    }
    of << "\n";

    for (const EmittedAssign& as : emittedAssigns) {
        of << "    v_" << as.m_lhsIdx << " = " << as.m_expr << ";  // "
           << as.m_lhsVarp->name() << "\n";
    }
    of << "\n";

    for (size_t i = 0; i < collector.vars().size(); ++i) {
        const AstVar* const varp = collector.vars().at(i);
        if (writtenVarIdxs.find(i) != writtenVarIdxs.end()) {
            of << "    state_out[" << i << "U * nstates + tid] = v_" << i << " & "
               << gemCudaMaskLiteral(varp->widthMin()) << ";\n";
        } else {
            of << "#ifndef SIM_ACCEL_PARTIAL_WRITE\n";
            of << "    state_out[" << i << "U * nstates + tid] = state_in[" << i
               << "U * nstates + tid] & " << gemCudaMaskLiteral(varp->widthMin()) << ";\n";
            of << "#endif\n";
        }
    }
    of << "}\n\n";

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_var_count() {\n"
       << "    return " << collector.vars().size() << "U;\n"
       << "}\n\n";

    of << "extern \"C\" __host__ const char* sim_accel_eval_var_name(uint32_t index) {\n"
       << "    switch (index) {\n";
    for (size_t i = 0; i < collector.vars().size(); ++i) {
        of << "    case " << i << "U: return \"" << gemCudaEscapeCString(collector.vars().at(i)->name())
           << "\";\n";
    }
    of << "    default: return \"\";\n"
       << "    }\n"
       << "}\n";

    of.close();

    const string metaFilename = filename + ".vars.tsv";
    std::ofstream met{metaFilename};
    if (met.is_open()) {
        met << "index\tname\thierarchy\tdirection\tis_primary_io\twidth\tis_activator\n";
        for (size_t i = 0; i < collector.vars().size(); ++i) {
            const AstVar* const varp = collector.vars().at(i);
            met << i << '\t' << varp->name() << '\t'
                << gemExtractHierarchy(varp->name()) << '\t'
                << varp->direction().ascii() << '\t'
                << (varp->isPrimaryIO() ? "1" : "0") << '\t'
                << varp->widthMin() << '\t'
                << (gemIsActivator(varp->name()) ? "1" : "0") << '\n';
        }
    }

    const string depsFilename = filename + ".deps.tsv";
    std::ofstream dep{depsFilename};
    if (dep.is_open()) {
        dep << "lhs_idx\trhs_idx_list\n";
        for (const EmittedAssign& as : emittedAssigns) {
            dep << as.m_lhsIdx << "\t";
            bool first = true;
            for (const size_t rhsIdx : as.m_rhsIdxs) {
                if (!first) dep << ",";
                dep << rhsIdx;
                first = false;
            }
            dep << "\n";
        }
    }

    {
        const double cov = totalAssignw ? (100.0 * static_cast<double>(supportedAssignw)
                                           / static_cast<double>(totalAssignw))
                                        : 0.0;
        std::ostringstream covStr;
        covStr << std::fixed << std::setprecision(2) << cov;
        v3info("--sim-accel-only stats "
               << "assignw_supported=" << supportedAssignw << " "
               << "assignw_total=" << totalAssignw << " "
               << "assignw_ignored=" << skipped << " "
               << "assignw_emitted_unique=" << emittedUniqueAssignw << " "
               << "assignw_coverage_pct=" << covStr.str());
    }
}
