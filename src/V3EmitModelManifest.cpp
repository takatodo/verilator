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

#include <algorithm>
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
        .end();
}

}  // namespace

void V3EmitModelManifest::emit() {
    UINFO(2, __FUNCTION__ << ":");
    const ManifestData data = CollectVisitor::collect(v3Global.rootp());
    V3Stats::addStat("Model manifest, Fields emitted", static_cast<double>(data.fields.size()));
    V3Stats::addStat("Model manifest, Instances emitted",
                     static_cast<double>(data.instances.size()));
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
    of.end()
        .begin("limitations")
        .put("semantic_id_stability", "experimental_not_guaranteed")
        .put("semantic_state_classification", "not_provided")
        .put("byte_offsets", "not_provided")
        .put("pointer_free_checkpoint", "not_provided")
        .put("generated_storage_instances", "provided")
        .put("semantic_instance_topology", "not_provided")
        .put("coverage_mapping", "not_provided")
        .put("eval_regions", "not_provided")
        .end();
}
