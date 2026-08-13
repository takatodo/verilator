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

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3EmitModelManifest.h"

#include "V3EmitCBase.h"
#include "V3File.h"

#include <algorithm>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

struct Field final {
    const AstNodeModule* modp;
    const AstVar* varp;
    std::string fieldId;
    std::string semanticPath;
};

bool isCompilerGenerated(const AstVar* varp) {
    return varp->isTemp() || varp->isGenVar() || varp->name().rfind("__V", 0) == 0;
}

std::string semanticPath(const AstVar* varp) {
    std::string path = varp->prettyName();
    if (path.find('.') == std::string::npos) {
        path = v3Global.rootp()->resolvedTopModuleName() + "." + path;
    }
    return path;
}

std::vector<Field> collectFields() {
    std::vector<Field> fields;
    for (const AstNodeModule* modp = v3Global.rootp()->modulesp(); modp;
         modp = VN_AS(modp->nextp(), NodeModule)) {
        for (const AstNode* nodep = modp->stmtsp(); nodep; nodep = nodep->nextp()) {
            const AstVar* const varp = VN_CAST(nodep, Var);
            if (!varp || !EmitCUtil::isEmittedDesignVar(varp)) continue;
            const bool generated = isCompilerGenerated(varp);
            const std::string path = generated ? "" : semanticPath(varp);
            const std::string id = generated ? "generated:" + EmitCUtil::prefixNameProtect(modp)
                                                   + "." + varp->nameProtect()
                                             : "rtl:" + path;
            fields.push_back(Field{modp, varp, id, path});
        }
    }
    std::stable_sort(fields.begin(), fields.end(), [](const Field& lhs, const Field& rhs) {
        if (lhs.fieldId != rhs.fieldId) return lhs.fieldId < rhs.fieldId;
        const std::string lhsClass = EmitCUtil::prefixNameProtect(lhs.modp);
        const std::string rhsClass = EmitCUtil::prefixNameProtect(rhs.modp);
        if (lhsClass != rhsClass) return lhsClass < rhsClass;
        return lhs.varp->nameProtect() < rhs.varp->nameProtect();
    });
    return fields;
}

void emitField(V3OutJsonFile& of, const Field& field) {
    const AstVar* const varp = field.varp;
    const FileLine* const fl = varp->fileline();
    const bool generated = isCompilerGenerated(varp);

    of.begin()
        .put("field_id", field.fieldId)
        .put("origin", generated ? "compiler_generated" : "rtl")
        .put("semantic_path", field.semanticPath)
        .put("rtl_name", generated ? "" : varp->origName())
        .put("width_bits", varp->widthMin())
        .put("direction", varp->direction().ascii())
        .put("var_type", varp->varType().ascii())
        .put("state_role", "unclassified")
        .begin("source")
        .put("file", fl->filename())
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
    const std::vector<Field> fields = collectFields();
    V3OutJsonFile of{v3Global.opt.modelManifestOutput()};

    of.put("schema_version", 1)
        .put("surface", "verilator_model_manifest_experimental")
        .put("producer", V3Options::version())
        .begin("model")
        .put("top", v3Global.rootp()->resolvedTopModuleName())
        .put("prefix", v3Global.opt.prefix())
        .end()
        .put("field_count", static_cast<int>(fields.size()))
        .begin("fields", '[');
    for (const Field& field : fields) emitField(of, field);
    of.end()
        .begin("limitations")
        .put("semantic_id_stability", "experimental_not_guaranteed")
        .put("semantic_state_classification", "not_provided")
        .put("byte_offsets", "not_provided")
        .put("pointer_free_checkpoint", "not_provided")
        .put("instance_topology", "not_provided")
        .put("coverage_mapping", "not_provided")
        .put("eval_regions", "not_provided")
        .end();
}
