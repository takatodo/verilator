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

struct SimAccelArrayElementKey final {
    const AstVar* m_varp = nullptr;
    int m_logicalIndex = 0;

    bool operator==(const SimAccelArrayElementKey& other) const {
        return m_varp == other.m_varp && m_logicalIndex == other.m_logicalIndex;
    }
};

struct SimAccelArrayElementKeyHash final {
    size_t operator()(const SimAccelArrayElementKey& key) const {
        const size_t varHash = std::hash<const AstVar*>{}(key.m_varp);
        const size_t indexHash = std::hash<int>{}(key.m_logicalIndex);
        return varHash ^ (indexHash + 0x9e3779b97f4a7c15ULL + (varHash << 6) + (varHash >> 2));
    }
};

bool simAccelIsInternalVar(const AstVar* varp) {
    return varp->name().compare(0, 3, "__V") == 0;
}

bool simAccelWidthSupported(const AstNode* nodep) {
    return nodep && nodep->widthMin() > 0 && nodep->widthMin() <= 32;
}

string simAccelNormalizeTargetPath(const string& name) {
    string out = name;
    size_t pos = 0;
    while ((pos = out.find("__DOT__", pos)) != string::npos) {
        out.replace(pos, 7, ".");
        pos += 1;
    }
    return out;
}

const AstNodeDType* simAccelLeafUnpackedElementDType(const AstNodeDType* dtypep) {
    for (const AstNodeDType* currentp = dtypep; currentp;) {
        currentp = currentp->skipRefp();
        if (const AstUnpackArrayDType* const unpackp = VN_CAST(currentp, UnpackArrayDType)) {
            currentp = unpackp->subDTypep();
            continue;
        }
        return currentp;
    }
    return nullptr;
}

bool simAccelIsSupportedScalarVar(const AstVar* varp) {
    return varp && !simAccelIsInternalVar(varp) && simAccelWidthSupported(varp) && varp->dtypep()
           && !VN_IS(varp->dtypep()->skipRefp(), UnpackArrayDType);
}

string simAccelSyntheticArrayElementName(const AstVar* varp, int logicalIndex) {
    return varp->name() + "__BRA__" + cvtToStr(logicalIndex) + "__KET__";
}

class SimAccelVarRefCollector final : public VNVisitorConst {
private:
    std::unordered_set<const AstVar*>& m_varps;

public:
    explicit SimAccelVarRefCollector(std::unordered_set<const AstVar*>& varps)
        : m_varps{varps} {}

    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }
    void visit(AstVarRef* nodep) override {
        const AstVar* const varp = nodep->varp();
        if (varp && !simAccelIsInternalVar(varp) && simAccelWidthSupported(varp)) {
            m_varps.emplace(varp);
        }
    }
};

void simAccelCollectNodeVars(const AstNode* nodep, std::unordered_set<const AstVar*>& varps) {
    if (!nodep) return;
    SimAccelVarRefCollector collector{varps};
    collector.iterateConst(const_cast<AstNode*>(nodep));
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
    struct ResolvedArrayElement final {
        const AstVar* m_varp = nullptr;
        int m_logicalIndex = 0;
        uint32_t m_width = 0;
    };

    V3SimAccelProgram m_program;
    std::unordered_map<const AstVar*, size_t> m_varToIndex;
    std::unordered_map<SimAccelArrayElementKey, size_t, SimAccelArrayElementKeyHash>
        m_arrayElementToIndex;
    std::vector<const AstVar*> m_indexToVar;
    std::unordered_set<size_t> m_gpuReadVarIdxs;
    std::unordered_set<size_t> m_gpuWrittenVarIdxs;
    std::unordered_set<const AstVar*> m_externalTouchedVarps;
    std::unordered_set<string> m_preloadTargetPaths;
    std::unordered_map<string, size_t> m_preloadTargetPathToIndex;
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
        var.m_isCpuVisible = var.m_isPrimaryIo;
        m_program.m_vars.push_back(std::move(var));
        m_varToIndex.emplace(varp, idx);
        m_indexToVar.push_back(varp);
        return idx;
    }

    bool resolveSupportedArrayElement(const AstNodeExpr* nodep, ResolvedArrayElement& resolved) const {
        const AstArraySel* const selp = VN_CAST(nodep, ArraySel);
        if (!selp) return false;

        const AstVarRef* const baseRefp = VN_CAST(selp->fromp(), VarRef);
        if (!baseRefp) return false;
        const AstVar* const baseVarp = baseRefp->varp();
        if (!baseVarp || simAccelIsInternalVar(baseVarp) || !baseVarp->dtypep()) return false;

        const AstUnpackArrayDType* const unpackp
            = VN_CAST(baseVarp->dtypep()->skipRefp(), UnpackArrayDType);
        if (!unpackp) return false;
        if (VN_IS(unpackp->subDTypep()->skipRefp(), UnpackArrayDType)) return false;

        const AstBasicDType* const basicp
            = VN_CAST(simAccelLeafUnpackedElementDType(unpackp->subDTypep()), BasicDType);
        if (!basicp || basicp->width() <= 0 || basicp->width() > 32) return false;

        const AstConst* const constp = VN_CAST(selp->bitp(), Const);
        if (!constp || constp->num().isFourState()) return false;

        const int64_t logicalIndex64 = constp->toSInt();
        const int logicalLo = unpackp->lo();
        const int logicalOffset = static_cast<int>(logicalIndex64) - logicalLo;
        if (logicalIndex64 < logicalLo || logicalOffset < 0
            || logicalOffset >= unpackp->elementsConst()) {
            return false;
        }

        resolved.m_varp = baseVarp;
        resolved.m_logicalIndex = static_cast<int>(logicalIndex64);
        resolved.m_width = basicp->width();
        return true;
    }

    size_t rememberArrayElement(const ResolvedArrayElement& resolved) {
        UASSERT_OBJ(resolved.m_varp, v3Global.rootp(), "Unexpected null sim-accel array element");
        const SimAccelArrayElementKey key{resolved.m_varp, resolved.m_logicalIndex};
        const auto it = m_arrayElementToIndex.find(key);
        if (it != m_arrayElementToIndex.end()) return it->second;

        const size_t idx = m_program.m_vars.size();
        V3SimAccelProgram::Var var;
        var.m_name = simAccelSyntheticArrayElementName(resolved.m_varp, resolved.m_logicalIndex);
        var.m_hierarchy = simAccelExtractHierarchy(resolved.m_varp->name());
        var.m_direction = resolved.m_varp->direction().ascii();
        var.m_width = resolved.m_width;
        var.m_isPrimaryIo = false;
        var.m_isActivator = simAccelIsActivator(resolved.m_varp->name());
        var.m_isCpuVisible = false;
        m_program.m_vars.push_back(std::move(var));
        m_arrayElementToIndex.emplace(key, idx);
        m_indexToVar.push_back(nullptr);
        rememberPreloadTargetElement(resolved, m_program.m_vars.at(idx).m_name);
        return idx;
    }

    void markExternalTouched(const AstNode* nodep) {
        simAccelCollectNodeVars(nodep, m_externalTouchedVarps);
    }

    void rememberPreloadTarget(const AstVar* varp) {
        if (!varp || simAccelIsInternalVar(varp) || !varp->dtypep()) return;
        const AstNodeDType* const dtypep = varp->dtypep()->skipRefp();
        if (!VN_IS(dtypep, UnpackArrayDType)) return;
        const AstNodeDType* const leafDTypep = simAccelLeafUnpackedElementDType(dtypep);
        const AstBasicDType* const basicp = VN_CAST(leafDTypep, BasicDType);
        if (!basicp || basicp->width() <= 0 || basicp->width() > 32) return;

        const string targetPath = simAccelNormalizeTargetPath(varp->name());
        if (!m_preloadTargetPaths.emplace(targetPath).second) return;

        V3SimAccelProgram::PreloadTarget target;
        target.m_kind = "memory-array-preload-v1";
        target.m_name = simAccelExtractBaseName(varp->name());
        target.m_targetPath = targetPath;
        target.m_astName = varp->name();
        target.m_hierarchy = simAccelExtractHierarchy(varp->name());
        target.m_wordBits = basicp->width();
        target.m_depth = varp->dtypep()->arrayUnpackedElements();
        target.m_baseAddr = 0;
        target.m_addressUnitBytes = std::max<uint32_t>(1, (target.m_wordBits + 7) / 8);
        target.m_endianness = "little";
        target.m_isPrimaryIo = varp->isPrimaryIO();
        const size_t targetIdx = m_program.m_preloadTargets.size();
        m_program.m_preloadTargets.push_back(std::move(target));
        m_preloadTargetPathToIndex.emplace(targetPath, targetIdx);
    }

    void rememberPreloadTargetElement(const ResolvedArrayElement& resolved, const string& varName) {
        if (!resolved.m_varp || !resolved.m_varp->dtypep()) return;
        const string targetPath = simAccelNormalizeTargetPath(resolved.m_varp->name());
        const auto targetIt = m_preloadTargetPathToIndex.find(targetPath);
        if (targetIt == m_preloadTargetPathToIndex.end()) return;

        const AstUnpackArrayDType* const unpackp
            = VN_CAST(resolved.m_varp->dtypep()->skipRefp(), UnpackArrayDType);
        if (!unpackp) return;
        const int logicalBase = unpackp->lo();
        const int relativeIndex = resolved.m_logicalIndex - logicalBase;
        if (relativeIndex < 0) return;

        V3SimAccelProgram::PreloadTarget& target = m_program.m_preloadTargets.at(targetIt->second);
        const uint32_t byteCount = std::max<uint32_t>(1, (resolved.m_width + 7) / 8);
        const uint32_t offset = static_cast<uint32_t>(relativeIndex) * byteCount;
        for (const V3SimAccelProgram::PreloadTarget::Element& existing : target.m_elements) {
            if (existing.m_index == resolved.m_logicalIndex || existing.m_varName == varName) return;
        }

        V3SimAccelProgram::PreloadTarget::Element element;
        element.m_index = resolved.m_logicalIndex;
        element.m_offset = offset;
        element.m_byteCount = byteCount;
        element.m_varName = varName;
        target.m_elements.push_back(std::move(element));
    }

    void finalizeCommPlan() {
        for (size_t idx = 0; idx < m_program.m_vars.size(); ++idx) {
            V3SimAccelProgram::Var& var = m_program.m_vars.at(idx);
            const bool isGpuRead = (m_gpuReadVarIdxs.find(idx) != m_gpuReadVarIdxs.end());
            const bool isGpuWritten = (m_gpuWrittenVarIdxs.find(idx) != m_gpuWrittenVarIdxs.end());
            const AstVar* const astVarp = idx < m_indexToVar.size() ? m_indexToVar.at(idx) : nullptr;
            const bool escapesExternally
                = var.m_isCpuVisible
                  || (astVarp && m_externalTouchedVarps.find(astVarp) != m_externalTouchedVarps.end());
            // Boundary inputs exclude GPU-local temporaries. Outputs require an external
            // consumer outside the supported GPU subset or a CPU-visible/public boundary.
            var.m_isGpuInput = isGpuRead && !isGpuWritten;
            var.m_isGpuOutput = isGpuWritten && escapesExternally;

            if (var.m_isGpuInput) {
                var.m_inputSlot = m_program.m_commPlan.m_cpuToGpuVarIdxs.size();
                m_program.m_commPlan.m_cpuToGpuVarIdxs.push_back(idx);
            }
            if (var.m_isGpuOutput) {
                var.m_outputSlot = m_program.m_commPlan.m_gpuToCpuVarIdxs.size();
                m_program.m_commPlan.m_gpuToCpuVarIdxs.push_back(idx);
            }
            if (var.m_isCpuVisible) m_program.m_commPlan.m_cpuVisibleVarIdxs.push_back(idx);
        }
    }

    bool isSupportedExpr(const AstNodeExpr* nodep) const {
        if (!nodep || !simAccelWidthSupported(nodep)) return false;
        ResolvedArrayElement resolvedArrayElement;
        if (resolveSupportedArrayElement(nodep, resolvedArrayElement)) return true;
        if (const AstVarRef* const refp = VN_CAST(nodep, VarRef)) {
            const AstVar* const varp = refp->varp();
            return simAccelIsSupportedScalarVar(varp);
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

    void collectExprVars(const AstNodeExpr* nodep, std::unordered_set<size_t>& currentRhsVars) {
        if (!nodep) return;
        ResolvedArrayElement resolvedArrayElement;
        if (resolveSupportedArrayElement(nodep, resolvedArrayElement)) {
            currentRhsVars.insert(rememberArrayElement(resolvedArrayElement));
            return;
        }
        if (const AstVarRef* const refp = VN_CAST(nodep, VarRef)) {
            const AstVar* const varp = refp->varp();
            if (simAccelIsSupportedScalarVar(varp)) {
                currentRhsVars.insert(rememberVar(varp));
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
        ResolvedArrayElement resolvedArrayElement;
        if (resolveSupportedArrayElement(nodep, resolvedArrayElement)) {
            expr.m_kind = V3SimAccelProgram::ExprKind::VAR;
            expr.m_varIdx = rememberArrayElement(resolvedArrayElement);
            return addExpr(std::move(expr));
        }
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
    void visit(AstVar* nodep) override {
        rememberPreloadTarget(nodep);
        iterateChildrenConst(nodep);
    }
    void visit(AstVarRef* nodep) override { markExternalTouched(nodep); }

    void visit(AstAssignW* nodep) override {
        if (nodep->timingControlp()) {
            markExternalTouched(nodep->lhsp());
            markExternalTouched(nodep->rhsp());
            ++m_skippedTiming;
            return;
        }
        AstVarRef* const lhsRefp = VN_CAST(nodep->lhsp(), VarRef);
        if (!lhsRefp) {
            markExternalTouched(nodep->lhsp());
            markExternalTouched(nodep->rhsp());
            ++m_skippedNonVarLhs;
            return;
        }
        AstVar* const lhsVarp = lhsRefp->varp();
        if (!lhsVarp || simAccelIsInternalVar(lhsVarp)) {
            markExternalTouched(nodep->rhsp());
            ++m_skippedInternal;
            return;
        }
        if (!simAccelWidthSupported(lhsVarp) || !isSupportedExpr(nodep->rhsp())) {
            markExternalTouched(nodep->lhsp());
            markExternalTouched(nodep->rhsp());
            ++m_skippedUnsupported;
            return;
        }

        const size_t lhsIdx = rememberVar(lhsVarp);
        std::unordered_set<size_t> rhsVars;
        collectExprVars(nodep->rhsp(), rhsVars);
        std::vector<size_t> rhsVarsVec{rhsVars.begin(), rhsVars.end()};
        std::sort(rhsVarsVec.begin(), rhsVarsVec.end());

        V3SimAccelProgram::Assign assign;
        assign.m_lhsIdx = lhsIdx;
        assign.m_lhsName = lhsVarp->name();
        assign.m_exprIdx = lowerExpr(nodep->rhsp());
        assign.m_rhsIdxs.reserve(rhsVarsVec.size());
        m_gpuWrittenVarIdxs.emplace(lhsIdx);
        for (const size_t rhsIdx : rhsVarsVec) {
            assign.m_rhsIdxs.push_back(rhsIdx);
            m_gpuReadVarIdxs.emplace(rhsIdx);
        }
        m_program.m_assigns.push_back(std::move(assign));
        ++m_program.m_stats.m_assignwSupported;
    }

    V3SimAccelProgram program() && {
        finalizeCommPlan();
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
