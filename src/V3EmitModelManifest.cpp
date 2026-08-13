// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Emit experimental semantic model manifest
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

// This emitter mirrors the generated class member selection used by
// V3EmitCHeaders and the module instance selection used by V3EmitCSyms.  It
// assigns deterministic identities, sorts both inventories, and writes them as
// JSON.  It does not infer semantic state roles or layout.

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3EmitModelManifest.h"

#include "V3EmitCBase.h"
#include "V3File.h"
#include "V3Stats.h"
#include "V3String.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

struct Field final {
    const AstNodeModule* modp;
    const AstVar* varp;
    std::string fieldId;
    std::string semanticPath;
};

struct Instance final {
    const AstScope* scopep;
    std::string instanceId;
    std::string parentInstanceId;
};

struct ManifestData final {
    std::vector<Field> fields;
    std::vector<Instance> instances;
};

struct ToggleTemplate final {
    AstCoverToggleDecl* declp;
    const AstNodeModule* modp;
    const AstCFunc* cfuncp;
    int ordinal;
};

struct ToggleUpdateTemplate final {
    AstCoverToggleDecl* declp;
    const AstNodeModule* modp;
    const AstCFunc* cfuncp;
};

struct CoverageCollection final {
    std::vector<AstNodeCoverDecl*> declarations;
    std::vector<ToggleTemplate> toggles;
    std::vector<ToggleUpdateTemplate> updates;
    int unsupportedDeclarations = 0;
};

struct ToggleMetadata final {
    std::string filename;
    std::string hierarchySuffix;
    std::string page;
    std::string comment;
    int line;
    int column;
    int begin;
    int end;
    bool ranged;
};

struct CoverageStorage final {
    std::string storageId;
    std::string semanticInstanceId;
    std::string container;
    std::string member;
    std::string storage;
    int wordCount;
};

struct CoverageDeclaration final {
    std::string loweringId;
    std::string semanticInstanceId;
    std::string storageId;
    ToggleMetadata metadata;
    int rawBaseWord;
    int templateOrdinal;
};

struct CoverageObservation final {
    std::string semanticId;
    std::string semanticInstanceId;
    ToggleMetadata metadata;
    int bitIndex;
    std::string transition;
};

struct CoverageBinding final {
    std::string semanticId;
    std::string loweringId;
    std::string physicalWordId;
};

struct CoveragePhysicalWord final {
    std::string physicalWordId;
    std::string storageId;
    std::string aliasGroupId;
    std::vector<std::string> memberSemanticIds;
    int rawWordIndex;
};

struct CoverageUpdateRegion final {
    std::string storageId;
    int rawBaseWord;
    int widthBits;
    int siteCount;
};

struct CoverageData final {
    std::string status;
    std::vector<CoverageStorage> storages;
    std::vector<CoverageDeclaration> declarations;
    std::vector<CoverageObservation> observations;
    std::vector<CoverageBinding> bindings;
    std::vector<CoveragePhysicalWord> physicalWords;
    std::vector<CoverageUpdateRegion> updateRegions;
    int toggleTemplateCount = 0;
    int updateTemplateCount = 0;
    int updateSiteCount = 0;
    int unsupportedDeclarationCount = 0;
    int uninstantiatedLocalDeclarationCount = 0;
    int unupdatedPhysicalWordCount = 0;
    int updateOnlyPhysicalWordCount = 0;
};

enum class EvalClassification : uint8_t { PROVEN_DEVICE_CLEAN, UNKNOWN, HOST_DEPENDENT };

struct EvalStateAccess final {
    std::string fieldId;
    int readSiteCount = 0;
    int writeSiteCount = 0;
};

struct EvalFunction final {
    const AstNodeModule* modp;
    const AstCFunc* funcp;
    std::string functionId;
    std::map<std::string, EvalStateAccess> stateAccessByField;
    std::map<const AstCFunc*, int> directCallSites;
    std::map<std::string, int> hostDependencySites;
    std::map<std::string, int> unknownEffectSites;
    EvalClassification directClassification = EvalClassification::PROVEN_DEVICE_CLEAN;
    EvalClassification classification = EvalClassification::PROVEN_DEVICE_CLEAN;
    int coverageUpdateSiteCount = 0;
};

struct EvalData final {
    std::string status;
    std::vector<EvalFunction> functions;
    std::string evalEntryFunctionId;
    int directCallEdgeCount = 0;
    int directCallSiteCount = 0;
    int stateAccessBindingCount = 0;
    int stateReadSiteCount = 0;
    int stateWriteSiteCount = 0;
    int coverageUpdateSiteCount = 0;
    int hostDependencySiteCount = 0;
    int unknownEffectSiteCount = 0;
};

std::string framedId(const std::string& kind,
                     const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string framed = kind;
    for (const auto& field : fields) {
        framed += std::to_string(field.first.size()) + ":" + field.first;
        framed += std::to_string(field.second.size()) + ":" + field.second;
    }
    VHashSha256 hash{framed};
    return kind + ":" + hash.digestHex();
}

const char* evalClassificationName(EvalClassification classification) {
    switch (classification) {
    case EvalClassification::PROVEN_DEVICE_CLEAN: return "proven_device_clean";
    case EvalClassification::UNKNOWN: return "unknown";
    case EvalClassification::HOST_DEPENDENT: return "host_dependent";
    }
    VL_UNREACHABLE;
    return "unknown";
}

EvalClassification evalClassification(const EvalFunction& function) {
    if (!function.hostDependencySites.empty()) return EvalClassification::HOST_DEPENDENT;
    if (!function.unknownEffectSites.empty()) return EvalClassification::UNKNOWN;
    return EvalClassification::PROVEN_DEVICE_CLEAN;
}

std::string evalFunctionId(const AstNodeModule* const modp, const AstCFunc* const funcp) {
    return framedId("eval-function:v1", {{"container", EmitCUtil::prefixNameProtect(modp)},
                                         {"name", funcp->nameProtect()},
                                         {"argument_types", funcp->argTypes()},
                                         {"return_type", funcp->rtnTypeVoid()}});
}

std::string evalRegionId(const std::string& functionId) {
    return framedId("eval-region:v1", {{"entry_function_id", functionId}});
}

template <typename T_Value>
int sumCounts(const std::map<std::string, T_Value>& values) {
    int total = 0;
    for (const auto& pair : values) total += pair.second;
    return total;
}

ToggleMetadata toggleMetadata(const AstCoverToggleDecl* const declp) {
    const FileLine* const fl = declp->fileline();
    return ToggleMetadata{
        VIdProtect::protect(fl->filename()),
        (!declp->hier().empty() ? "." : "")
            + VIdProtect::protectWordsIf(declp->hier(), declp->protect()),
        VIdProtect::protectWordsIf(declp->page(), declp->protect()),
        VIdProtect::protectWordsIf(declp->comment(), declp->protect()),
        fl->lineno(),
        fl->firstColumn(),
        declp->range().right(),
        declp->range().left(),
        declp->range().ranged(),
    };
}

std::string storageId(const std::string& semanticInstanceId, const std::string& container,
                      const std::string& member, const std::string& storage) {
    return framedId("coverage-storage:v1", {{"semantic_instance_id", semanticInstanceId},
                                            {"container", container},
                                            {"member", member},
                                            {"storage", storage}});
}

std::string physicalWordId(const std::string& storage, int rawWordIndex) {
    return framedId("coverage-word:v1",
                    {{"storage_id", storage}, {"raw_word_index", std::to_string(rawWordIndex)}});
}

std::string loweringId(const ToggleMetadata& metadata, const std::string& semanticInstanceId,
                       int templateOrdinal) {
    return framedId("toggle-lowering:v1", {{"semantic_instance_id", semanticInstanceId},
                                           {"filename", metadata.filename},
                                           {"line", std::to_string(metadata.line)},
                                           {"column", std::to_string(metadata.column)},
                                           {"hierarchy_suffix", metadata.hierarchySuffix},
                                           {"page", metadata.page},
                                           {"comment", metadata.comment},
                                           {"begin", std::to_string(metadata.begin)},
                                           {"end", std::to_string(metadata.end)},
                                           {"ranged", metadata.ranged ? "true" : "false"},
                                           {"template_ordinal", std::to_string(templateOrdinal)}});
}

std::string semanticToggleId(const ToggleMetadata& metadata, const std::string& semanticInstanceId,
                             int bitIndex, const std::string& transition) {
    return framedId("toggle-observation:v1",
                    {{"semantic_instance_id", semanticInstanceId},
                     {"filename", metadata.filename},
                     {"line", std::to_string(metadata.line)},
                     {"column", std::to_string(metadata.column)},
                     {"hierarchy_suffix", metadata.hierarchySuffix},
                     {"page", metadata.page},
                     {"comment", metadata.comment},
                     {"bit_index", metadata.ranged ? std::to_string(bitIndex) : "not_applicable"},
                     {"transition", transition}});
}

std::string aliasGroupId(const std::vector<std::string>& memberSemanticIds) {
    std::vector<std::pair<std::string, std::string>> fields;
    fields.reserve(memberSemanticIds.size());
    for (const std::string& member : memberSemanticIds) fields.emplace_back("member", member);
    return framedId("coverage-alias-group:v1", fields);
}

bool isCompilerGenerated(const AstVar* const varp) {
    return varp->isTemp() || varp->isGenVar() || varp->isInternal();
}

std::string semanticPath(const AstNodeModule* const modp, const AstVar* const varp) {
    std::string path = varp->prettyName();
    if (path.find('.') == std::string::npos) {
        const std::string modulePath
            = modp->isTop() ? v3Global.rootp()->resolvedTopModuleName() : modp->prettyName();
        path = modulePath + "." + path;
    }
    return VIdProtect::protectWordsIf(path, v3Global.opt.protectIds());
}

std::string instancePath(const AstScope* const scopep) {
    const std::string path
        = scopep->isTop() ? v3Global.rootp()->resolvedTopModuleName() : scopep->prettyName();
    return VIdProtect::protectWordsIf(path, scopep->protect());
}

class CollectVisitor final : public VNVisitorConst {
    const AstNodeModule* m_modp = nullptr;
    ManifestData m_data;

    void visit(AstNodeModule* nodep) override {
        VL_RESTORER(m_modp);
        m_modp = nodep;
        iterateChildrenConst(nodep);
    }
    void visit(AstVar* nodep) override {
        if (!EmitCUtil::isEmittedDesignVar(nodep)) return;
        UASSERT_OBJ(m_modp, nodep, "Model manifest variable is not under a module");
        const bool generated = isCompilerGenerated(nodep);
        const std::string path = generated ? "" : semanticPath(m_modp, nodep);
        const std::string id = generated ? "generated:" + EmitCUtil::prefixNameProtect(m_modp)
                                               + "." + nodep->nameProtect()
                                         : "rtl:" + path;
        m_data.fields.push_back(Field{m_modp, nodep, id, path});
    }
    void visit(AstScope* nodep) override {
        if (!VN_IS(nodep->modp(), Class)) {
            const std::string id = "rtl_instance:" + instancePath(nodep);
            const std::string parentId
                = nodep->aboveScopep() ? "rtl_instance:" + instancePath(nodep->aboveScopep()) : "";
            m_data.instances.push_back(Instance{nodep, id, parentId});
        }
        iterateChildrenConst(nodep);
    }
    void visit(AstCFunc*) override {}
    void visit(AstConstPool*) override {}
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

    explicit CollectVisitor(AstNetlist* const netlistp) { iterateConst(netlistp); }

public:
    static ManifestData collect(AstNetlist* const netlistp) {
        CollectVisitor visitor{netlistp};
        std::stable_sort(visitor.m_data.fields.begin(), visitor.m_data.fields.end(),
                         [](const Field& lhs, const Field& rhs) {
                             if (lhs.fieldId != rhs.fieldId) return lhs.fieldId < rhs.fieldId;
                             const std::string lhsClass = EmitCUtil::prefixNameProtect(lhs.modp);
                             const std::string rhsClass = EmitCUtil::prefixNameProtect(rhs.modp);
                             if (lhsClass != rhsClass) return lhsClass < rhsClass;
                             return lhs.varp->nameProtect() < rhs.varp->nameProtect();
                         });
        std::stable_sort(visitor.m_data.instances.begin(), visitor.m_data.instances.end(),
                         [](const Instance& lhs, const Instance& rhs) {
                             if (lhs.instanceId != rhs.instanceId) {
                                 return lhs.instanceId < rhs.instanceId;
                             }
                             return lhs.scopep->nameDotless() < rhs.scopep->nameDotless();
                         });
        return std::move(visitor.m_data);
    }
};

class EvalCollectVisitor final : public VNVisitorConst {
    std::map<const AstVar*, std::string> m_fieldIds;
    const AstNodeModule* m_modp = nullptr;
    const AstCFunc* m_cfuncp = nullptr;
    std::map<const AstCFunc*, size_t> m_functionIndex;
    EvalData m_data;

    EvalFunction& currentFunction() {
        UASSERT(m_cfuncp, "Eval effect found outside a generated function");
        return m_data.functions.at(m_functionIndex.at(m_cfuncp));
    }

    void addHostDependency(const std::string& category) {
        if (m_cfuncp) ++currentFunction().hostDependencySites[category];
    }
    void addUnknownEffect(const std::string& kind) {
        if (m_cfuncp) ++currentFunction().unknownEffectSites[kind];
    }

    void recordFunctionFlags(AstCFunc* const nodep) {
        if (nodep->dpiContext() || nodep->dpiExportDispatcher() || nodep->dpiExportImpl()
            || nodep->dpiImportPrototype() || nodep->dpiImportWrapper()
            || nodep->dpiCDeclOverride()) {
            addHostDependency("dpi_vpi");
        }
        if (nodep->needProcess() || nodep->isCoroutine()) addHostDependency("coroutine");
        if (nodep->isConstructor() || nodep->isDestructor()) addHostDependency("allocation");
        if (nodep->isTrace()) addHostDependency("io");
        if (nodep->isVirtual()) addUnknownEffect("virtual_function");
        if (nodep->recursive()) addUnknownEffect("recursive_function");
    }

    void recordCall(AstNodeCCall* const nodep) {
        if (nodep->funcp()) {
            ++currentFunction().directCallSites[nodep->funcp()];
        } else {
            addUnknownEffect("unresolved_call");
        }
    }

    void recordHardMethod(AstCMethodHard* const nodep) {
        const VCMethod::en method = nodep->method().m_e;
        if (method >= VCMethod::ARRAY_AND && method <= VCMethod::DYN_SLICE_FRONT_BACK) {
            addHostDependency("dynamic_storage");
        } else if (method >= VCMethod::EVENT_CLEAR_FIRED
                   && method <= VCMethod::EVENT_IS_TRIGGERED) {
            addHostDependency("coroutine");
        } else if (method >= VCMethod::FORCE_ADD && method <= VCMethod::FORCE_TOUCH) {
            addHostDependency("runtime_context");
        } else if (method >= VCMethod::FORK_DONE && method <= VCMethod::FORK_ON_KILL) {
            addHostDependency("coroutine");
        } else if (method >= VCMethod::RANDOMIZER_BASIC_STD_RANDOMIZATION
                   && method <= VCMethod::RNG_SET_RANDSTATE) {
            addHostDependency("runtime_context");
        } else if (method >= VCMethod::SCHED_ANY_TRIGGERED && method <= VCMethod::SCHED_TRIGGER) {
            addHostDependency("scheduler");
        } else if (method < VCMethod::UNPACKED_ASSIGN || method > VCMethod::UNPACKED_NEQ) {
            addUnknownEffect("unclassified_runtime_method");
        }
    }

    void visit(AstNodeModule* nodep) override {
        VL_RESTORER(m_modp);
        m_modp = nodep;
        iterateChildrenConst(nodep);
    }
    void visit(AstCFunc* nodep) override {
        UASSERT_OBJ(m_modp, nodep, "Generated function is not under a module");
        const size_t index = m_data.functions.size();
        const std::string id = evalFunctionId(m_modp, nodep);
        const auto pair = m_functionIndex.emplace(nodep, index);
        UASSERT_OBJ(pair.second, nodep, "Duplicate generated function in eval manifest");
        m_data.functions.push_back(EvalFunction{m_modp, nodep, id});
        VL_RESTORER(m_cfuncp);
        m_cfuncp = nodep;
        recordFunctionFlags(nodep);
        iterateChildrenConst(nodep);
    }
    void visit(AstNodeVarRef* nodep) override {
        if (!m_cfuncp) return;
        AstVar* const varp = nodep->varp();
        if (!varp) {
            addUnknownEffect("unresolved_variable_reference");
            return;
        }
        const auto fieldIt = m_fieldIds.find(varp);
        if (fieldIt != m_fieldIds.end()) {
            EvalStateAccess& access
                = currentFunction()
                      .stateAccessByField
                      .emplace(fieldIt->second, EvalStateAccess{fieldIt->second})
                      .first->second;
            if (nodep->access().isReadOrRW()) ++access.readSiteCount;
            if (nodep->access().isWriteOrRW()) ++access.writeSiteCount;
        } else if (!varp->isFuncLocalSticky() && !varp->isParam() && !varp->isConst()) {
            addUnknownEffect("unmapped_nonlocal_variable_reference");
        }
    }
    void visit(AstNodeCCall* nodep) override {
        recordCall(nodep);
        iterateChildrenConst(nodep);
    }
    void visit(AstCNew* nodep) override {
        addHostDependency("allocation");
        recordCall(nodep);
        iterateChildrenConst(nodep);
    }
    void visit(AstCMethodHard* nodep) override {
        recordHardMethod(nodep);
        iterateChildrenConst(nodep);
    }
    void visit(AstCoverInc* nodep) override {
        ++currentFunction().coverageUpdateSiteCount;
        iterateChildrenConst(nodep);
    }
    void visit(AstCStmt* nodep) override {
        addUnknownEffect("raw_cpp_statement");
        iterateChildrenConst(nodep);
    }
    void visit(AstCStmtUser* nodep) override {
        addUnknownEffect("user_cpp_statement");
        iterateChildrenConst(nodep);
    }
    void visit(AstCExpr* nodep) override {
        addUnknownEffect("raw_cpp_expression");
        iterateChildrenConst(nodep);
    }
    void visit(AstCExprUser* nodep) override {
        addUnknownEffect("user_cpp_expression");
        iterateChildrenConst(nodep);
    }
    void visit(AstCAwait* nodep) override {
        addHostDependency("coroutine");
        iterateChildrenConst(nodep);
    }
    void visit(AstFireEvent* nodep) override {
        addHostDependency("coroutine");
        iterateChildrenConst(nodep);
    }
    void visit(AstSFormat* nodep) override {
        addHostDependency("io");
        iterateChildrenConst(nodep);
    }
    void visit(AstSFormatF* nodep) override {
        addHostDependency("io");
        iterateChildrenConst(nodep);
    }
    void visit(AstStop* nodep) override {
        addHostDependency("termination");
        iterateChildrenConst(nodep);
    }
    void visit(AstFinish* nodep) override {
        addHostDependency("termination");
        iterateChildrenConst(nodep);
    }
    void visit(AstFinishFork* nodep) override {
        addHostDependency("termination");
        iterateChildrenConst(nodep);
    }
    void visit(AstTime* nodep) override {
        addHostDependency("time");
        iterateChildrenConst(nodep);
    }
    void visit(AstTimeD* nodep) override {
        addHostDependency("time");
        iterateChildrenConst(nodep);
    }
    void visit(AstTimeImport* nodep) override {
        addHostDependency("time");
        iterateChildrenConst(nodep);
    }
    void visit(AstConstPool*) override {}
    void visit(AstNode* nodep) override {
        if (m_cfuncp) {
            if (nodep->isTimingControl()) {
                addHostDependency("scheduler");
            } else if (nodep->isOutputter()) {
                addHostDependency("io");
            } else if (nodep->isSystemFunc()) {
                addUnknownEffect("unclassified_system_operation");
            } else if (VN_IS(nodep, NodeExpr) && !nodep->isPure()) {
                addUnknownEffect("unclassified_effectful_expression");
            } else if (VN_IS(nodep, NodeStmt) && !VN_IS(nodep, StmtExpr) && !nodep->isPure()) {
                addUnknownEffect("unclassified_effectful_statement");
            }
        }
        iterateChildrenConst(nodep);
    }

    EvalCollectVisitor(AstNetlist* const netlistp, const ManifestData& manifestData) {
        for (const Field& field : manifestData.fields) {
            const auto pair = m_fieldIds.emplace(field.varp, field.fieldId);
            UASSERT(pair.second || pair.first->second == field.fieldId,
                    "Conflicting model manifest field identity");
        }
        iterateConst(netlistp);
    }

public:
    static EvalData collect(AstNetlist* const netlistp, const ManifestData& manifestData) {
        EvalCollectVisitor visitor{netlistp, manifestData};
        EvalData& data = visitor.m_data;
        std::stable_sort(data.functions.begin(), data.functions.end(),
                         [](const EvalFunction& lhs, const EvalFunction& rhs) {
                             if (lhs.functionId != rhs.functionId) {
                                 return lhs.functionId < rhs.functionId;
                             }
                             if (lhs.funcp->nameProtect() != rhs.funcp->nameProtect()) {
                                 return lhs.funcp->nameProtect() < rhs.funcp->nameProtect();
                             }
                             return EmitCUtil::prefixNameProtect(lhs.modp)
                                    < EmitCUtil::prefixNameProtect(rhs.modp);
                         });

        std::map<const AstCFunc*, size_t> functionIndex;
        std::map<std::string, const AstCFunc*> functionById;
        for (size_t index = 0; index < data.functions.size(); ++index) {
            EvalFunction& function = data.functions[index];
            UASSERT(functionIndex.emplace(function.funcp, index).second,
                    "Duplicate eval function pointer");
            const auto idPair = functionById.emplace(function.functionId, function.funcp);
            UASSERT(idPair.second || idPair.first->second == function.funcp,
                    "Conflicting eval function identity");
        }
        for (EvalFunction& function : data.functions) {
            for (const auto& call : function.directCallSites) {
                if (functionIndex.find(call.first) == functionIndex.end()) {
                    function.unknownEffectSites["callee_not_in_function_inventory"] += call.second;
                }
            }
            function.directClassification = evalClassification(function);
            function.classification = function.directClassification;
        }

        bool changed = false;
        do {
            changed = false;
            for (EvalFunction& function : data.functions) {
                EvalClassification next = function.directClassification;
                for (const auto& call : function.directCallSites) {
                    const auto indexIt = functionIndex.find(call.first);
                    if (indexIt == functionIndex.end()) continue;
                    next = std::max(next, data.functions[indexIt->second].classification);
                }
                if (next != function.classification) {
                    function.classification = next;
                    changed = true;
                }
            }
        } while (changed);

        int evalEntryCount = 0;
        for (const EvalFunction& function : data.functions) {
            if (!function.funcp->isEvalEntry()) continue;
            ++evalEntryCount;
            data.evalEntryFunctionId = function.functionId;
        }
        if (evalEntryCount == 0) {
            data.status = "not_present";
        } else if (evalEntryCount != 1) {
            data.status = "partial";
            data.evalEntryFunctionId.clear();
        } else {
            data.status = "provided";
        }
        for (const EvalFunction& function : data.functions) {
            for (const auto& call : function.directCallSites) {
                if (functionIndex.find(call.first) == functionIndex.end()) continue;
                ++data.directCallEdgeCount;
                data.directCallSiteCount += call.second;
            }
            data.stateAccessBindingCount += static_cast<int>(function.stateAccessByField.size());
            for (const auto& access : function.stateAccessByField) {
                data.stateReadSiteCount += access.second.readSiteCount;
                data.stateWriteSiteCount += access.second.writeSiteCount;
            }
            data.coverageUpdateSiteCount += function.coverageUpdateSiteCount;
            data.hostDependencySiteCount += sumCounts(function.hostDependencySites);
            data.unknownEffectSiteCount += sumCounts(function.unknownEffectSites);
        }
        return std::move(data);
    }
};

class CoverageCollectVisitor final : public VNVisitorConst {
    const AstNodeModule* m_modp = nullptr;
    const AstCFunc* m_cfuncp = nullptr;
    CoverageCollection m_data;

    void visit(AstNodeModule* nodep) override {
        VL_RESTORER(m_modp);
        m_modp = nodep;
        iterateChildrenConst(nodep);
    }
    void visit(AstCFunc* nodep) override {
        VL_RESTORER(m_cfuncp);
        m_cfuncp = nodep;
        iterateChildrenConst(nodep);
    }
    void visit(AstCoverToggleDecl* nodep) override {
        UASSERT_OBJ(m_modp, nodep, "Toggle coverage declaration is not under a module");
        m_data.declarations.push_back(nodep);
        m_data.toggles.push_back(
            ToggleTemplate{nodep, m_modp, m_cfuncp, static_cast<int>(m_data.toggles.size())});
    }
    void visit(AstNodeCoverDecl* nodep) override {
        m_data.declarations.push_back(nodep);
        ++m_data.unsupportedDeclarations;
    }
    void visit(AstCoverInc* nodep) override {
        if (AstCoverToggleDecl* const declp = VN_CAST(nodep->declp(), CoverToggleDecl)) {
            UASSERT_OBJ(m_modp, nodep, "Toggle coverage update is not under a module");
            m_data.updates.push_back(ToggleUpdateTemplate{declp, m_modp, m_cfuncp});
        }
        iterateChildrenConst(nodep);
    }
    void visit(AstConstPool*) override {}
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

    explicit CoverageCollectVisitor(AstNetlist* const netlistp) { iterateConst(netlistp); }

public:
    static CoverageCollection collect(AstNetlist* const netlistp) {
        CoverageCollectVisitor visitor{netlistp};
        return std::move(visitor.m_data);
    }
};

CoverageStorage makeLocalCoverageStorage(const Instance& instance, int wordCount) {
    const AstScope* const scopep = instance.scopep;
    const std::string container = EmitCUtil::prefixNameProtect(scopep->modp());
    const std::string member = "__Vcoverage";
    const std::string storage = "instance_member";
    return CoverageStorage{storageId(instance.instanceId, container, member, storage),
                           instance.instanceId,
                           container,
                           member,
                           storage,
                           wordCount};
}

CoverageStorage makeGlobalCoverageStorage(int wordCount) {
    const std::string container = EmitCUtil::symClassName();
    const std::string member = "__Vcoverage";
    const std::string storage = "symbol_table_member";
    return CoverageStorage{
        storageId("", container, member, storage), "", container, member, storage, wordCount};
}

using CoverageLocation = std::pair<std::string, int>;
using CoverageRegionKey = std::tuple<std::string, int, int>;

struct CoverageBuildState final {
    CoverageData data;
    std::map<std::string, CoverageStorage> storageById;
    std::map<std::string, CoverageObservation> observationById;
    std::map<std::string, CoverageLocation> locationByPhysicalWord;
    std::map<std::string, std::set<std::string>> membersByPhysicalWord;
    std::map<CoverageRegionKey, int> updateRegionCounts;
    std::set<std::string> updatedPhysicalWords;
};

void addStorage(CoverageBuildState& state, const CoverageStorage& storage) {
    const auto pair = state.storageById.emplace(storage.storageId, storage);
    if (!pair.second) {
        UASSERT(pair.first->second.semanticInstanceId == storage.semanticInstanceId
                    && pair.first->second.container == storage.container
                    && pair.first->second.member == storage.member
                    && pair.first->second.storage == storage.storage
                    && pair.first->second.wordCount == storage.wordCount,
                "Conflicting model manifest coverage storage identity");
    }
}

void addPhysicalWord(CoverageBuildState& state, const std::string& storage, int rawWordIndex,
                     const std::string& semanticId) {
    const auto storageIt = state.storageById.find(storage);
    UASSERT(storageIt != state.storageById.end(), "Coverage word refers to unknown storage");
    UASSERT(rawWordIndex >= 0 && rawWordIndex < storageIt->second.wordCount,
            "Coverage word lies outside generated storage");
    const std::string physicalId = physicalWordId(storage, rawWordIndex);
    const CoverageLocation expected{storage, rawWordIndex};
    const auto pair = state.locationByPhysicalWord.emplace(physicalId, expected);
    UASSERT(pair.second || pair.first->second == expected,
            "Conflicting model manifest physical coverage word identity");
    state.membersByPhysicalWord[physicalId].emplace(semanticId);
}

void addObservation(CoverageBuildState& state, const CoverageDeclaration& declaration,
                    int bitIndex, int directionOffset, const std::string& transition) {
    const ToggleMetadata& metadata = declaration.metadata;
    const std::string semanticId
        = semanticToggleId(metadata, declaration.semanticInstanceId, bitIndex, transition);
    const CoverageObservation observation{semanticId, declaration.semanticInstanceId, metadata,
                                          bitIndex, transition};
    const auto pair = state.observationById.emplace(semanticId, observation);
    if (!pair.second) {
        const CoverageObservation& previous = pair.first->second;
        UASSERT(previous.semanticInstanceId == observation.semanticInstanceId
                    && previous.metadata.filename == observation.metadata.filename
                    && previous.metadata.line == observation.metadata.line
                    && previous.metadata.column == observation.metadata.column
                    && previous.metadata.hierarchySuffix == observation.metadata.hierarchySuffix
                    && previous.metadata.page == observation.metadata.page
                    && previous.metadata.comment == observation.metadata.comment
                    && previous.metadata.ranged == observation.metadata.ranged
                    && previous.bitIndex == observation.bitIndex
                    && previous.transition == observation.transition,
                "Conflicting model manifest semantic coverage identity");
    }
    const int begin = metadata.begin;
    const int step = metadata.end >= begin ? 1 : -1;
    const int bitOrdinal = step > 0 ? bitIndex - begin : begin - bitIndex;
    const int rawWordIndex = declaration.rawBaseWord + bitOrdinal * 2 + directionOffset;
    const std::string physicalId = physicalWordId(declaration.storageId, rawWordIndex);
    state.data.bindings.push_back(CoverageBinding{semanticId, declaration.loweringId, physicalId});
    addPhysicalWord(state, declaration.storageId, rawWordIndex, semanticId);
}

CoverageStorage coverageStorageFor(const Instance* const instancep, int localWordCount,
                                   int globalWordCount) {
    return instancep ? makeLocalCoverageStorage(*instancep, localWordCount)
                     : makeGlobalCoverageStorage(globalWordCount);
}

void addDeclarationOccurrence(CoverageBuildState& state, const ToggleTemplate& toggle,
                              const Instance* const instancep, int localWordCount,
                              int globalWordCount) {
    const bool local = instancep != nullptr;
    CoverageStorage storage = coverageStorageFor(instancep, localWordCount, globalWordCount);
    addStorage(state, storage);
    const ToggleMetadata metadata = toggleMetadata(toggle.declp);
    const std::string semanticInstanceId = local ? instancep->instanceId : "";
    const int rawBaseWord = local ? toggle.declp->dataDeclThisp()->localBinNum()
                                  : toggle.declp->dataDeclThisp()->binNum();
    UASSERT(rawBaseWord >= 0 && rawBaseWord + toggle.declp->size() <= storage.wordCount,
            "Toggle declaration lies outside generated coverage storage");
    const CoverageDeclaration declaration{loweringId(metadata, semanticInstanceId, toggle.ordinal),
                                          semanticInstanceId,
                                          storage.storageId,
                                          metadata,
                                          rawBaseWord,
                                          toggle.ordinal};
    state.data.declarations.push_back(declaration);

    const int step = metadata.end >= metadata.begin ? 1 : -1;
    int bitIndex = metadata.begin;
    while (true) {
        addObservation(state, declaration, bitIndex, 0, "1->0");
        addObservation(state, declaration, bitIndex, 1, "0->1");
        if (bitIndex == metadata.end) break;
        bitIndex += step;
    }
}

void addUpdateOccurrence(CoverageBuildState& state, const ToggleUpdateTemplate& update,
                         const Instance* const instancep, int localWordCount,
                         int globalWordCount) {
    const bool local = instancep != nullptr;
    CoverageStorage storage = coverageStorageFor(instancep, localWordCount, globalWordCount);
    addStorage(state, storage);
    const int rawBaseWord = local ? update.declp->dataDeclThisp()->localBinNum()
                                  : update.declp->dataDeclThisp()->binNum();
    const int widthBits = update.declp->size() / 2;
    UASSERT(rawBaseWord >= 0 && rawBaseWord + 2 * widthBits <= storage.wordCount,
            "Toggle update lies outside generated coverage storage");
    ++state.updateRegionCounts[CoverageRegionKey{storage.storageId, rawBaseWord, widthBits}];
    ++state.data.updateSiteCount;
    for (int offset = 0; offset < 2 * widthBits; ++offset) {
        state.updatedPhysicalWords.emplace(
            physicalWordId(storage.storageId, rawBaseWord + offset));
    }
}

CoverageData buildCoverageData(const CoverageCollection& collection,
                               const std::vector<Instance>& instances, int globalWordCount) {
    CoverageBuildState state;
    state.data.toggleTemplateCount = static_cast<int>(collection.toggles.size());
    state.data.updateTemplateCount = static_cast<int>(collection.updates.size());
    state.data.unsupportedDeclarationCount = collection.unsupportedDeclarations;

    std::map<const AstNodeModule*, std::vector<const Instance*>> instancesByModule;
    for (const Instance& instance : instances) {
        instancesByModule[instance.scopep->modp()].push_back(&instance);
    }
    std::map<const AstNodeModule*, int> localWordCounts;
    for (AstNodeCoverDecl* const declp : collection.declarations) {
        if (declp->dataDeclNullp()) continue;
        AstNodeCoverDecl* const representativep = declp->dataDeclThisp();
        const AstNodeModule* const ownerp = EmitCParentModule::get(representativep);
        int& count = localWordCounts[ownerp];
        count = std::max(count, representativep->localBinNum() + representativep->size());
    }

    for (const ToggleTemplate& toggle : collection.toggles) {
        const bool local
            = EmitCUtil::coverageUsesLocalCounter(toggle.cfuncp, toggle.modp, toggle.declp);
        if (!local) {
            addDeclarationOccurrence(state, toggle, nullptr, 0, globalWordCount);
            continue;
        }
        const auto instanceIt = instancesByModule.find(toggle.modp);
        if (instanceIt == instancesByModule.end() || instanceIt->second.empty()) {
            ++state.data.uninstantiatedLocalDeclarationCount;
            continue;
        }
        const auto countIt = localWordCounts.find(toggle.modp);
        UASSERT(countIt != localWordCounts.end(), "Covered module has no local counter layout");
        for (const Instance* const instancep : instanceIt->second) {
            addDeclarationOccurrence(state, toggle, instancep, countIt->second, globalWordCount);
        }
    }

    for (const ToggleUpdateTemplate& update : collection.updates) {
        const bool local
            = EmitCUtil::coverageUsesLocalCounter(update.cfuncp, update.modp, update.declp);
        if (!local) {
            addUpdateOccurrence(state, update, nullptr, 0, globalWordCount);
            continue;
        }
        const auto instanceIt = instancesByModule.find(update.modp);
        if (instanceIt == instancesByModule.end()) continue;
        const auto countIt = localWordCounts.find(update.modp);
        UASSERT(countIt != localWordCounts.end(), "Covered module has no local counter layout");
        for (const Instance* const instancep : instanceIt->second) {
            addUpdateOccurrence(state, update, instancep, countIt->second, globalWordCount);
        }
    }

    for (const auto& pair : state.storageById) state.data.storages.push_back(pair.second);
    for (const auto& pair : state.observationById) state.data.observations.push_back(pair.second);
    std::stable_sort(state.data.declarations.begin(), state.data.declarations.end(),
                     [](const CoverageDeclaration& lhs, const CoverageDeclaration& rhs) {
                         return lhs.loweringId < rhs.loweringId;
                     });
    std::stable_sort(state.data.bindings.begin(), state.data.bindings.end(),
                     [](const CoverageBinding& lhs, const CoverageBinding& rhs) {
                         return std::tie(lhs.semanticId, lhs.physicalWordId, lhs.loweringId)
                                < std::tie(rhs.semanticId, rhs.physicalWordId, rhs.loweringId);
                     });
    state.data.bindings.erase(
        std::unique(state.data.bindings.begin(), state.data.bindings.end(),
                    [](const CoverageBinding& lhs, const CoverageBinding& rhs) {
                        return std::tie(lhs.semanticId, lhs.physicalWordId, lhs.loweringId)
                               == std::tie(rhs.semanticId, rhs.physicalWordId, rhs.loweringId);
                    }),
        state.data.bindings.end());

    for (const auto& pair : state.membersByPhysicalWord) {
        const auto locationIt = state.locationByPhysicalWord.find(pair.first);
        UASSERT(locationIt != state.locationByPhysicalWord.end(),
                "Coverage alias members have no physical location");
        std::vector<std::string> members{pair.second.begin(), pair.second.end()};
        state.data.physicalWords.push_back(
            CoveragePhysicalWord{pair.first, locationIt->second.first, aliasGroupId(members),
                                 std::move(members), locationIt->second.second});
    }
    std::stable_sort(state.data.physicalWords.begin(), state.data.physicalWords.end(),
                     [](const CoveragePhysicalWord& lhs, const CoveragePhysicalWord& rhs) {
                         return std::tie(lhs.storageId, lhs.rawWordIndex)
                                < std::tie(rhs.storageId, rhs.rawWordIndex);
                     });
    for (const auto& pair : state.updateRegionCounts) {
        state.data.updateRegions.push_back(
            CoverageUpdateRegion{std::get<0>(pair.first), std::get<1>(pair.first),
                                 std::get<2>(pair.first), pair.second});
    }

    for (const auto& pair : state.locationByPhysicalWord) {
        if (state.updatedPhysicalWords.find(pair.first) == state.updatedPhysicalWords.end()) {
            ++state.data.unupdatedPhysicalWordCount;
        }
    }
    for (const std::string& physicalId : state.updatedPhysicalWords) {
        if (state.locationByPhysicalWord.find(physicalId) == state.locationByPhysicalWord.end()) {
            ++state.data.updateOnlyPhysicalWordCount;
        }
    }

    if (collection.toggles.empty() && collection.unsupportedDeclarations == 0) {
        state.data.status = "not_present";
    } else if (collection.toggles.empty()) {
        state.data.status = "not_provided";
    } else if (collection.unsupportedDeclarations || state.data.unupdatedPhysicalWordCount
               || state.data.updateOnlyPhysicalWordCount) {
        state.data.status = "partial";
    } else {
        state.data.status = "provided";
    }
    return std::move(state.data);
}

void emitToggleMetadata(V3OutJsonFile& of, const ToggleMetadata& metadata) {
    of.begin("source")
        .put("file", metadata.filename)
        .put("line", metadata.line)
        .put("column", metadata.column)
        .end()
        .put("hierarchy_suffix", metadata.hierarchySuffix)
        .put("page", metadata.page)
        .put("comment", metadata.comment);
}

void emitCoverageStorage(V3OutJsonFile& of, const CoverageStorage& storage) {
    of.begin()
        .put("storage_id", storage.storageId)
        .put("semantic_instance_id", storage.semanticInstanceId)
        .put("word_bits", 32)
        .put("word_count", storage.wordCount)
        .begin("generated_binding")
        .put("container", storage.container)
        .put("member", storage.member)
        .put("storage", storage.storage)
        .end()
        .end();
}

void emitCoverageDeclaration(V3OutJsonFile& of, const CoverageDeclaration& declaration) {
    of.begin()
        .put("lowering_id", declaration.loweringId)
        .put("semantic_instance_id", declaration.semanticInstanceId)
        .put("storage_id", declaration.storageId)
        .put("raw_base_word", declaration.rawBaseWord)
        .put("template_ordinal", declaration.templateOrdinal);
    emitToggleMetadata(of, declaration.metadata);
    of.begin("range")
        .put("begin", declaration.metadata.begin)
        .put("end", declaration.metadata.end)
        .put("ranged", declaration.metadata.ranged)
        .end()
        .end();
}

void emitCoverageObservation(V3OutJsonFile& of, const CoverageObservation& observation) {
    of.begin()
        .put("semantic_id", observation.semanticId)
        .put("semantic_instance_id", observation.semanticInstanceId);
    emitToggleMetadata(of, observation.metadata);
    if (observation.metadata.ranged) {
        of.put("bit_index", observation.bitIndex);
    } else {
        of.put("bit_index_status", "not_applicable");
    }
    of.put("transition", observation.transition).end();
}

void emitCoverage(V3OutJsonFile& of, const CoverageData& coverage) {
    const int aliasedWords = static_cast<int>(std::count_if(
        coverage.physicalWords.begin(), coverage.physicalWords.end(),
        [](const CoveragePhysicalWord& word) { return word.memberSemanticIds.size() > 1; }));
    size_t maximumMembers = 0;
    for (const CoveragePhysicalWord& word : coverage.physicalWords) {
        maximumMembers = std::max(maximumMembers, word.memberSemanticIds.size());
    }

    of.begin("coverage")
        .put("status", coverage.status)
        .put("authority", "verilator_coverage_lowering")
        .put("kind", "toggle_transition")
        .put("semantic_id_scheme", "sha256_length_prefixed_utf8_v1")
        .put("physical_id_scheme", "sha256_length_prefixed_utf8_v1")
        .begin("counter_semantics")
        .put("word_bits", 32)
        .put("cpp_type", v3Global.opt.threads() > 1 ? "std::atomic<uint32_t>" : "uint32_t")
        .put("hit", "nonzero_word")
        .put("alias_aggregation", "logical_or")
        .begin("transition_order", '[')
        .put(std::string{"1->0"})
        .put(std::string{"0->1"})
        .end()
        .end()
        .begin("metrics")
        .put("toggle_template_count", coverage.toggleTemplateCount)
        .put("lowering_declaration_count", static_cast<int>(coverage.declarations.size()))
        .put("semantic_observation_count", static_cast<int>(coverage.observations.size()))
        .put("semantic_binding_count", static_cast<int>(coverage.bindings.size()))
        .put("storage_count", static_cast<int>(coverage.storages.size()))
        .put("physical_word_count", static_cast<int>(coverage.physicalWords.size()))
        .put("aliased_physical_word_count", aliasedWords)
        .put("maximum_semantic_observations_per_physical_word", static_cast<int>(maximumMembers))
        .put("update_template_count", coverage.updateTemplateCount)
        .put("update_site_count", coverage.updateSiteCount)
        .put("update_region_count", static_cast<int>(coverage.updateRegions.size()))
        .put("unsupported_declaration_count", coverage.unsupportedDeclarationCount)
        .put("uninstantiated_local_declaration_count",
             coverage.uninstantiatedLocalDeclarationCount)
        .put("unupdated_physical_word_count", coverage.unupdatedPhysicalWordCount)
        .put("update_only_physical_word_count", coverage.updateOnlyPhysicalWordCount)
        .end()
        .begin("storages", '[');
    for (const CoverageStorage& storage : coverage.storages) emitCoverageStorage(of, storage);
    of.end().begin("lowering_declarations", '[');
    for (const CoverageDeclaration& declaration : coverage.declarations) {
        emitCoverageDeclaration(of, declaration);
    }
    of.end().begin("semantic_observations", '[');
    for (const CoverageObservation& observation : coverage.observations) {
        emitCoverageObservation(of, observation);
    }
    of.end().begin("bindings", '[');
    for (const CoverageBinding& binding : coverage.bindings) {
        of.begin()
            .put("semantic_id", binding.semanticId)
            .put("lowering_id", binding.loweringId)
            .put("physical_word_id", binding.physicalWordId)
            .end();
    }
    of.end().begin("physical_words", '[');
    for (const CoveragePhysicalWord& word : coverage.physicalWords) {
        of.begin()
            .put("physical_word_id", word.physicalWordId)
            .put("storage_id", word.storageId)
            .put("raw_word_index", word.rawWordIndex)
            .put("alias_group_id", word.aliasGroupId)
            .put("member_count", static_cast<int>(word.memberSemanticIds.size()))
            .put("hit_aggregation",
                 word.memberSemanticIds.size() == 1 ? "direct" : "logical_or_alias")
            .begin("member_semantic_ids", '[');
        for (const std::string& semanticId : word.memberSemanticIds) of.put(semanticId);
        of.end();
        of.end();
    }
    of.end().begin("update_regions", '[');
    for (const CoverageUpdateRegion& region : coverage.updateRegions) {
        of.begin()
            .put("storage_id", region.storageId)
            .put("raw_base_word", region.rawBaseWord)
            .put("width_bits", region.widthBits)
            .put("site_count", region.siteCount)
            .end();
    }
    of.end()
        .begin("non_claims", '[')
        .put(std::string{"non_toggle_coverage_types_are_not_mapped"})
        .put(std::string{"generated_byte_offsets_are_not_provided"})
        .put(std::string{"an_aliased_word_cannot_attribute_a_hit_to_one_member"})
        .put(std::string{"semantic_ids_are_experimental_across_verilator_versions"})
        .end()
        .end();
}

const char* evalClassificationReason(const EvalFunction& function) {
    if (function.classification == EvalClassification::HOST_DEPENDENT) {
        return function.directClassification == EvalClassification::HOST_DEPENDENT
                   ? "direct_host_dependency"
                   : "transitive_host_dependency";
    }
    if (function.classification == EvalClassification::UNKNOWN) {
        return function.directClassification == EvalClassification::UNKNOWN
                   ? "direct_unknown_effect"
                   : "transitive_unknown_effect";
    }
    return "no_host_or_unknown_effect_in_final_ast_closure";
}

void emitEvalFunction(V3OutJsonFile& of, const EvalFunction& function,
                      const std::map<const AstCFunc*, std::string>& functionIds) {
    std::vector<std::pair<std::string, int>> calls;
    for (const auto& call : function.directCallSites) {
        const auto idIt = functionIds.find(call.first);
        if (idIt != functionIds.end()) calls.emplace_back(idIt->second, call.second);
    }
    std::stable_sort(calls.begin(), calls.end());

    const AstCFunc* const funcp = function.funcp;
    of.begin()
        .put("function_id", function.functionId)
        .begin("generated_binding")
        .put("container", EmitCUtil::prefixNameProtect(function.modp))
        .put("name", funcp->nameProtect())
        .put("kind", funcp->isProperMethod() ? "method" : "loose_function")
        .end()
        .put("is_eval_entry", funcp->isEvalEntry())
        .put("entry_point", funcp->entryPoint())
        .put("slow", funcp->slow())
        .begin("direct_state_accesses", '[');
    for (const auto& accessPair : function.stateAccessByField) {
        const EvalStateAccess& access = accessPair.second;
        of.begin()
            .put("field_id", access.fieldId)
            .put("read_site_count", access.readSiteCount)
            .put("write_site_count", access.writeSiteCount)
            .end();
    }
    of.end().begin("direct_calls", '[');
    for (const auto& call : calls) {
        of.begin().put("callee_function_id", call.first).put("site_count", call.second).end();
    }
    of.end()
        .begin("direct_effects")
        .put("coverage_update_site_count", function.coverageUpdateSiteCount)
        .begin("host_dependencies", '[');
    for (const auto& dependency : function.hostDependencySites) {
        of.begin().put("category", dependency.first).put("site_count", dependency.second).end();
    }
    of.end().begin("unknown_effects", '[');
    for (const auto& effect : function.unknownEffectSites) {
        of.begin().put("kind", effect.first).put("site_count", effect.second).end();
    }
    of.end()
        .end()
        .put("direct_classification", evalClassificationName(function.directClassification))
        .put("classification", evalClassificationName(function.classification))
        .put("reason", evalClassificationReason(function))
        .end();
}

void emitEvalRegions(V3OutJsonFile& of, const EvalData& eval) {
    std::map<const AstCFunc*, std::string> functionIds;
    const EvalFunction* entryFunctionp = nullptr;
    int directClean = 0;
    int directUnknown = 0;
    int directHost = 0;
    int clean = 0;
    int unknown = 0;
    int host = 0;
    for (const EvalFunction& function : eval.functions) {
        functionIds.emplace(function.funcp, function.functionId);
        if (function.functionId == eval.evalEntryFunctionId) entryFunctionp = &function;
        switch (function.directClassification) {
        case EvalClassification::PROVEN_DEVICE_CLEAN: ++directClean; break;
        case EvalClassification::UNKNOWN: ++directUnknown; break;
        case EvalClassification::HOST_DEPENDENT: ++directHost; break;
        }
        switch (function.classification) {
        case EvalClassification::PROVEN_DEVICE_CLEAN: ++clean; break;
        case EvalClassification::UNKNOWN: ++unknown; break;
        case EvalClassification::HOST_DEPENDENT: ++host; break;
        }
    }

    of.begin("eval_regions")
        .put("status", eval.status)
        .put("authority", "verilator_final_ast")
        .put("function_id_scheme", "sha256_length_prefixed_utf8_v1")
        .put("region_id_scheme", "sha256_length_prefixed_utf8_v1")
        .begin("classification_policy")
        .put("policy", "conservative_final_ast_effects_v1")
        .begin("precedence", '[')
        .put(std::string{"host_dependent"})
        .put(std::string{"unknown"})
        .put(std::string{"proven_device_clean"})
        .end()
        .put("propagation", "transitive_call_closure_fixed_point")
        .end()
        .begin("metrics")
        .put("function_count", static_cast<int>(eval.functions.size()))
        .put("region_count", entryFunctionp ? 1 : 0)
        .put("direct_call_edge_count", eval.directCallEdgeCount)
        .put("direct_call_site_count", eval.directCallSiteCount)
        .put("state_access_binding_count", eval.stateAccessBindingCount)
        .put("state_read_site_count", eval.stateReadSiteCount)
        .put("state_write_site_count", eval.stateWriteSiteCount)
        .put("coverage_update_site_count", eval.coverageUpdateSiteCount)
        .put("host_dependency_site_count", eval.hostDependencySiteCount)
        .put("unknown_effect_site_count", eval.unknownEffectSiteCount)
        .put("direct_proven_device_clean_function_count", directClean)
        .put("direct_unknown_function_count", directUnknown)
        .put("direct_host_dependent_function_count", directHost)
        .put("proven_device_clean_function_count", clean)
        .put("unknown_function_count", unknown)
        .put("host_dependent_function_count", host)
        .end()
        .begin("functions", '[');
    for (const EvalFunction& function : eval.functions) {
        emitEvalFunction(of, function, functionIds);
    }
    of.end().begin("regions", '[');
    if (entryFunctionp) {
        of.begin()
            .put("region_id", evalRegionId(entryFunctionp->functionId))
            .put("kind", "main_eval")
            .put("entry_function_id", entryFunctionp->functionId)
            .put("classification", evalClassificationName(entryFunctionp->classification))
            .put("reason", evalClassificationReason(*entryFunctionp))
            .put("dependency_graph", "function_direct_calls")
            .put("schedule_semantics", "not_provided")
            .put("convergence_semantics", "not_provided")
            .end();
    }
    of.end()
        .begin("non_claims", '[')
        .put(std::string{"manually_emitted_eval_step_wrapper_is_not_classified"})
        .put(std::string{"schedule_and_convergence_semantics_are_not_provided"})
        .put(std::string{"definition_field_accesses_are_not_instance_occurrence_projections"})
        .put(std::string{"device_backend_codegen_is_not_provided"})
        .end()
        .end();
}

void emitInstance(V3OutJsonFile& of, const Instance& instance) {
    const AstScope* const scopep = instance.scopep;
    of.begin()
        .put("instance_id", instance.instanceId)
        .put("semantic_path", instancePath(scopep))
        .put("parent_instance_id", instance.parentInstanceId)
        .put("is_top", scopep->isTop())
        .begin("module_binding")
        .put("container", EmitCUtil::prefixNameProtect(scopep->modp()))
        .end()
        .begin("generated_binding")
        .put("container", EmitCUtil::symClassName())
        .put("member", VIdProtect::protectIf(scopep->nameDotless(), scopep->protect()))
        .put("storage", "instance_member")
        .end()
        .end();
}

void emitField(V3OutJsonFile& of, const Field& field) {
    const AstVar* const varp = field.varp;
    const FileLine* const fl = varp->fileline();
    const bool generated = isCompilerGenerated(varp);
    const EmitCUtil::SavableFieldKind savableKind = EmitCUtil::savableFieldKind(field.modp, varp);

    of.begin()
        .put("field_id", field.fieldId)
        .put("origin", generated ? "compiler_generated" : "rtl")
        .put("semantic_path", field.semanticPath)
        .put("rtl_name", generated ? "" : VIdProtect::protect(varp->origName()))
        .put("width_bits", varp->widthMin())
        .put("direction", varp->direction().ascii())
        .put("var_type", varp->varType().ascii())
        .put("state_role", "unclassified")
        .begin("source")
        .put("file", VIdProtect::protect(fl->filename()))
        .put("first_line", fl->firstLineno())
        .put("first_column", fl->firstColumn())
        .put("last_line", fl->lastLineno())
        .put("last_column", fl->lastColumn())
        .end()
        .begin("generated_binding")
        .put("container", EmitCUtil::prefixNameProtect(field.modp))
        .put("member", varp->nameProtect())
        .put("storage", varp->isStatic() ? "static_member" : "instance_member")
        .put("alignment_bytes", varp->dtypeSkipRefp()->widthAlignBytes())
        .end()
        .begin("initialization")
        .put("needs_c_reset", varp->needsCReset())
        .put("has_user_init", varp->hasUserInit())
        .put("no_reset", varp->noReset())
        .end()
        .begin("checkpoint_membership")
        .put("status",
             savableKind == EmitCUtil::SavableFieldKind::INCLUDED ? "included" : "excluded")
        .put("authority", "verilator_savable_field_selection")
        .put("reason", EmitCUtil::savableFieldKindAscii(savableKind))
        .end()
        .end();
}

}  // namespace

void V3EmitModelManifest::emit() {
    UINFO(2, __FUNCTION__ << ":");
    const int globalCoverageWords = EmitCUtil::assignCoverageBinNumbers(v3Global.rootp());
    const EmitCParentModule emitCParentModule;
    const ManifestData data = CollectVisitor::collect(v3Global.rootp());
    const EvalData eval = EvalCollectVisitor::collect(v3Global.rootp(), data);
    const CoverageCollection coverageCollection
        = CoverageCollectVisitor::collect(v3Global.rootp());
    const CoverageData coverage
        = buildCoverageData(coverageCollection, data.instances, globalCoverageWords);
    const size_t checkpointIncluded
        = std::count_if(data.fields.begin(), data.fields.end(), [](const Field& field) {
              return EmitCUtil::savableFieldKind(field.modp, field.varp)
                     == EmitCUtil::SavableFieldKind::INCLUDED;
          });
    V3Stats::addStat("Model manifest, Fields emitted", static_cast<double>(data.fields.size()));
    V3Stats::addStat("Model manifest, Instances emitted",
                     static_cast<double>(data.instances.size()));
    V3Stats::addStat("Model manifest, Checkpoint fields included",
                     static_cast<double>(checkpointIncluded));
    V3Stats::addStat("Model manifest, Coverage observations emitted",
                     static_cast<double>(coverage.observations.size()));
    V3Stats::addStat("Model manifest, Coverage physical words emitted",
                     static_cast<double>(coverage.physicalWords.size()));
    V3Stats::addStat("Model manifest, Eval functions emitted",
                     static_cast<double>(eval.functions.size()));
    V3Stats::addStat("Model manifest, Eval regions emitted",
                     eval.evalEntryFunctionId.empty() ? 0.0 : 1.0);
    V3OutJsonFile of{v3Global.opt.modelManifestOutput()};

    of.put("schema_version", 1)
        .put("surface", "verilator_model_manifest_experimental")
        .put("producer", V3Options::version())
        .begin("model")
        .put("top", VIdProtect::protect(v3Global.rootp()->resolvedTopModuleName()))
        .put("prefix", v3Global.opt.prefix())
        .end()
        .put("field_count", static_cast<int>(data.fields.size()))
        .begin("fields", '[');
    for (const Field& field : data.fields) emitField(of, field);
    of.end()
        .put("instance_count", static_cast<int>(data.instances.size()))
        .begin("instances", '[');
    for (const Instance& instance : data.instances) emitInstance(of, instance);
    of.end();
    emitCoverage(of, coverage);
    emitEvalRegions(of, eval);
    of.begin("checkpoint_projection")
        .put("status", "field_membership_only")
        .put("authority", "verilator_savable_field_selection")
        .put("included_definition_field_count", static_cast<int>(checkpointIncluded))
        .put("excluded_definition_field_count",
             static_cast<int>(data.fields.size() - checkpointIncluded))
        .put("runtime_state", "not_provided")
        .put("packing", "not_provided")
        .end()
        .begin("limitations")
        .put("semantic_id_stability", "experimental_not_guaranteed")
        .put("semantic_state_classification", "not_provided")
        .put("byte_offsets", "not_provided")
        .put("pointer_free_checkpoint", "not_provided")
        .put("checkpoint_field_membership", "provided")
        .put("generated_storage_instances", "provided")
        .put("semantic_instance_topology", "not_provided")
        .put("coverage_mapping", coverage.status)
        .put("eval_regions", eval.status)
        .end();
}
