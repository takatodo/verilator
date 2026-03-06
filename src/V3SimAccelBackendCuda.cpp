// DESCRIPTION: Verilator: Sim-Accel CUDA Backend
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

#include "V3SimAccelBackendCuda.h"

#include "V3Error.h"
#include "V3Global.h"
#include "V3Options.h"
#include "V3SimAccelLowerAssignwBool32.h"
#include "V3SimAccelProgram.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>

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

string simAccelEscapeCString(const string& in) {
    string out;
    out.reserve(in.size() + 8);
    for (char ch : in) {
        if (ch == '\\' || ch == '"') out += '\\';
        out += ch;
    }
    return out;
}

class SimAccelCudaExprEmitter final {
    const V3SimAccelProgram& m_program;

    string maskWrap(const string& expr, uint32_t width) const {
        if (width >= 32) return "(" + expr + ")";
        return "((" + expr + ") & " + simAccelMaskLiteral(width) + ")";
    }

public:
    explicit SimAccelCudaExprEmitter(const V3SimAccelProgram& program)
        : m_program{program} {}

    string emit(size_t exprIdx) const {
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
};

void emitStats(const V3SimAccelProgram& program, size_t emittedUniqueAssignw) {
    const size_t supportedAssignw = program.m_stats.m_assignwSupported;
    const size_t totalAssignw = program.m_stats.m_assignwTotal;
    const size_t skipped = program.m_stats.m_assignwIgnored;
    if (skipped) {
        v3info("--sim-accel-only ignored " << skipped
                                          << " ASSIGNW nodes (unsupported or internal constructs)");
    }
    const double cov = totalAssignw
                           ? (100.0 * static_cast<double>(supportedAssignw)
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

}  // namespace

void V3SimAccelBackendCuda::emitCuda() VL_MT_DISABLED {
    const string strategy = v3Global.opt.simAccelStrategy();
    if (strategy != "assignw-bool32") {
        v3fatal("Unsupported sim-accel CUDA strategy: " + strategy);  // LCOV_EXCL_LINE
    }
    const string filename = (v3Global.opt.simAccelOutput().empty()
                                 ? v3Global.opt.makeDir() + "/" + v3Global.opt.prefix()
                                       + ".sim_accel.kernel.cu"
                                 : v3Global.opt.simAccelOutput());
    if (v3Global.opt.simAccelSplitModules()) {
        v3fatal("--sim-accel-split-modules is not implemented yet");  // LCOV_EXCL_LINE
    }

    const V3SimAccelProgram program = V3SimAccelLowerAssignwBool32::build();
    if (program.m_assigns.empty()) {
        v3fatal("No supported ASSIGNW nodes found for --sim-accel-only "
                "(supported expr ops: VARREF/CONST/AND/OR/XOR/LOGAND/LOGOR/LOGNOT/NOT/ADD/SUB/"
                "EQ/NEQ/COND/CONCAT/SEL/SHIFT plus CCAST/EXTEND wrappers)");
    }

    std::ofstream of{filename};
    if (!of.is_open()) v3fatal("Cannot open output file: " + filename);  // LCOV_EXCL_LINE

    of << "// Generated by Verilator --sim-accel-only\n"
       << "// Supported node subset: ASSIGNW + bool/arith/select/shift/concat (+ casts/extends)\n"
       << "#include <stdint.h>\n\n";

    of << "extern \"C\" __global__ void sim_accel_eval_assignw_u32(const uint32_t* state_in,\n"
       << "                                                  uint32_t* state_out,\n"
       << "                                                  uint32_t nstates) {\n"
       << "    const uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;\n"
       << "    if (tid >= nstates) return;\n";

    struct EmittedAssign final {
        size_t m_lhsIdx = 0;
        string m_lhsName;
        string m_expr;
        std::vector<size_t> m_rhsIdxs;
    };
    const SimAccelCudaExprEmitter emitter{program};
    std::unordered_set<string> emittedAssignKeys;
    std::vector<EmittedAssign> emittedAssigns;
    std::unordered_set<size_t> writtenVarIdxs;
    std::unordered_set<size_t> readVarIdxs;

    for (const V3SimAccelProgram::Assign& assign : program.m_assigns) {
        const string expr = emitter.emit(assign.m_exprIdx);
        if (expr == ("v_" + cvtToStr(assign.m_lhsIdx))) continue;
        const string key = cvtToStr(assign.m_lhsIdx) + "|" + expr;
        if (!emittedAssignKeys.emplace(key).second) continue;

        EmittedAssign rec;
        rec.m_lhsIdx = assign.m_lhsIdx;
        rec.m_lhsName = assign.m_lhsName;
        rec.m_expr = expr;
        rec.m_rhsIdxs = assign.m_rhsIdxs;
        for (const size_t rhsIdx : rec.m_rhsIdxs) readVarIdxs.emplace(rhsIdx);
        writtenVarIdxs.emplace(rec.m_lhsIdx);
        emittedAssigns.push_back(std::move(rec));
    }

    for (size_t i = 0; i < program.m_vars.size(); ++i) {
        const V3SimAccelProgram::Var& var = program.m_vars.at(i);
        if (readVarIdxs.find(i) != readVarIdxs.end()) {
            of << "    uint32_t v_" << i << " = state_in[" << i << "U * nstates + tid] & "
               << simAccelMaskLiteral(var.m_width) << ";  // " << var.m_name << "\n";
        } else if (writtenVarIdxs.find(i) != writtenVarIdxs.end()) {
            of << "    uint32_t v_" << i << " = 0u;  // " << var.m_name << " (write-only)\n";
        }
    }
    of << "\n";

    for (const EmittedAssign& assign : emittedAssigns) {
        of << "    v_" << assign.m_lhsIdx << " = " << assign.m_expr << ";  // "
           << assign.m_lhsName << "\n";
    }
    of << "\n";

    for (size_t i = 0; i < program.m_vars.size(); ++i) {
        const V3SimAccelProgram::Var& var = program.m_vars.at(i);
        if (writtenVarIdxs.find(i) != writtenVarIdxs.end()) {
            of << "    state_out[" << i << "U * nstates + tid] = v_" << i << " & "
               << simAccelMaskLiteral(var.m_width) << ";\n";
        } else {
            of << "#ifndef SIM_ACCEL_PARTIAL_WRITE\n";
            of << "    state_out[" << i << "U * nstates + tid] = state_in[" << i
               << "U * nstates + tid] & " << simAccelMaskLiteral(var.m_width) << ";\n";
            of << "#endif\n";
        }
    }
    of << "}\n\n";

    of << "extern \"C\" __host__ uint32_t sim_accel_eval_var_count() {\n"
       << "    return " << program.m_vars.size() << "U;\n"
       << "}\n\n";

    of << "extern \"C\" __host__ const char* sim_accel_eval_var_name(uint32_t index) {\n"
       << "    switch (index) {\n";
    for (size_t i = 0; i < program.m_vars.size(); ++i) {
        of << "    case " << i << "U: return \""
           << simAccelEscapeCString(program.m_vars.at(i).m_name) << "\";\n";
    }
    of << "    default: return \"\";\n"
       << "    }\n"
       << "}\n";
    of.close();

    const string metaFilename = filename + ".vars.tsv";
    std::ofstream met{metaFilename};
    if (met.is_open()) {
        met << "index\tname\thierarchy\tdirection\tis_primary_io\twidth\tis_activator\n";
        for (size_t i = 0; i < program.m_vars.size(); ++i) {
            const V3SimAccelProgram::Var& var = program.m_vars.at(i);
            met << i << '\t' << var.m_name << '\t' << var.m_hierarchy << '\t'
                << var.m_direction << '\t' << (var.m_isPrimaryIo ? "1" : "0") << '\t'
                << var.m_width << '\t' << (var.m_isActivator ? "1" : "0") << '\n';
        }
    }

    const string depsFilename = filename + ".deps.tsv";
    std::ofstream dep{depsFilename};
    if (dep.is_open()) {
        dep << "lhs_idx\trhs_idx_list\n";
        for (const EmittedAssign& assign : emittedAssigns) {
            dep << assign.m_lhsIdx << "\t";
            bool first = true;
            for (const size_t rhsIdx : assign.m_rhsIdxs) {
                if (!first) dep << ",";
                dep << rhsIdx;
                first = false;
            }
            dep << "\n";
        }
    }

    emitStats(program, emittedAssigns.size());
}
