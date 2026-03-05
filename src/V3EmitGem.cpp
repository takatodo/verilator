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

#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

void V3EmitGem::emitGemIr() VL_MT_DISABLED {
    const string filename = (v3Global.opt.gemIrOutput().empty()
                                 ? v3Global.opt.makeDir() + "/" + v3Global.opt.prefix()
                                       + ".gem.tree.json"
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

class GemCudaCollector final : public VNVisitorConst {
public:
    struct AssignRec final {
        AstVar* m_lhsVarp = nullptr;
        const AstNodeExpr* m_rhsp = nullptr;
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

    void collectExprVars(const AstNodeExpr* nodep) {
        if (!nodep) return;
        if (const AstVarRef* const refp = VN_CAST(nodep, VarRef)) {
            rememberVar(refp->varp());
            return;
        }
        if (const AstAnd* const andp = VN_CAST(nodep, And)) {
            collectExprVars(andp->lhsp());
            collectExprVars(andp->rhsp());
            return;
        }
        if (const AstOr* const orp = VN_CAST(nodep, Or)) {
            collectExprVars(orp->lhsp());
            collectExprVars(orp->rhsp());
            return;
        }
        if (const AstXor* const xorp = VN_CAST(nodep, Xor)) {
            collectExprVars(xorp->lhsp());
            collectExprVars(xorp->rhsp());
            return;
        }
        if (const AstNot* const notp = VN_CAST(nodep, Not)) {
            collectExprVars(notp->lhsp());
            return;
        }
        if (const AstCCast* const castp = VN_CAST(nodep, CCast)) {
            collectExprVars(castp->lhsp());
            return;
        }
        if (const AstExtend* const extp = VN_CAST(nodep, Extend)) {
            collectExprVars(extp->lhsp());
            return;
        }
        if (const AstExtendS* const extp = VN_CAST(nodep, ExtendS)) {
            collectExprVars(extp->lhsp());
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
        collectExprVars(nodep->rhsp());
        m_assigns.push_back({lhsVarp, nodep->rhsp()});
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
        nodep->v3fatalSrc("Unsupported node for --gem-cuda-only: " << nodep->typeName());
        VL_UNREACHABLE;
    }
};

}  // namespace

void V3EmitGem::emitGemCuda() VL_MT_DISABLED {
    const string filename = (v3Global.opt.gemCudaOutput().empty()
                                 ? v3Global.opt.makeDir() + "/" + v3Global.opt.prefix()
                                       + ".gem.kernel.cu"
                                 : v3Global.opt.gemCudaOutput());

    const GemCudaCollector collector{v3Global.rootp()};
    if (collector.assigns().empty()) {
        v3fatal("No supported ASSIGNW nodes found for --gem-cuda-only "  // LCOV_EXCL_LINE
                "(supported expr ops: VARREF/CONST/AND/OR/XOR/NOT plus CCAST/EXTEND wrappers)");
    }

    const size_t skipped = collector.skippedUnsupported() + collector.skippedInternal()
                           + collector.skippedNonVarLhs() + collector.skippedTiming();
    if (skipped) {
        v3info("--gem-cuda-only ignored " << skipped
                                          << " ASSIGNW nodes (unsupported or internal "
                                             "constructs)");
    }

    std::ofstream of{filename};
    if (!of.is_open()) v3fatal("Cannot open output file: " + filename);  // LCOV_EXCL_LINE

    of << "// Generated by Verilator --gem-cuda-only\n"
       << "// Supported node subset: ASSIGNW + AND/OR/XOR/NOT (+ casts/extends)\n"
       << "#include <stdint.h>\n\n";

    of << "extern \"C\" __global__ void gem_eval_assignw_u32(const uint32_t* state_in,\n"
       << "                                                  uint32_t* state_out,\n"
       << "                                                  uint32_t nstates) {\n"
       << "    const uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;\n"
       << "    if (tid >= nstates) return;\n";

    for (size_t i = 0; i < collector.vars().size(); ++i) {
        const AstVar* const varp = collector.vars().at(i);
        of << "    uint32_t v_" << i << " = state_in[" << i << "U * nstates + tid] & "
           << gemCudaMaskLiteral(varp->widthMin()) << ";  // " << varp->name() << "\n";
    }
    of << "\n";

    const GemCudaExprEmitter emitter{collector.varToIndex()};
    std::unordered_set<string> emittedAssignKeys;
    for (const GemCudaCollector::AssignRec& as : collector.assigns()) {
        const size_t lhsIdx = collector.varToIndex().at(as.m_lhsVarp);
        const string expr = emitter.emit(as.m_rhsp);
        const string key = cvtToStr(lhsIdx) + "|" + expr;
        if (!emittedAssignKeys.emplace(key).second) continue;
        of << "    v_" << lhsIdx << " = " << expr << ";  // "
           << as.m_lhsVarp->name() << "\n";
    }
    of << "\n";

    for (size_t i = 0; i < collector.vars().size(); ++i) {
        const AstVar* const varp = collector.vars().at(i);
        of << "    state_out[" << i << "U * nstates + tid] = v_" << i << " & "
           << gemCudaMaskLiteral(varp->widthMin()) << ";\n";
    }
    of << "}\n\n";

    of << "extern \"C\" __host__ uint32_t gem_eval_var_count() {\n"
       << "    return " << collector.vars().size() << "U;\n"
       << "}\n\n";

    of << "extern \"C\" __host__ const char* gem_eval_var_name(uint32_t index) {\n"
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
        met << "index\tname\tdirection\tis_primary_io\twidth\n";
        for (size_t i = 0; i < collector.vars().size(); ++i) {
            const AstVar* const varp = collector.vars().at(i);
            met << i << '\t' << varp->name() << '\t' << varp->direction().ascii() << '\t'
                << (varp->isPrimaryIO() ? "1" : "0") << '\t' << varp->widthMin() << '\n';
        }
    }
}
