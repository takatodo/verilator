// DESCRIPTION: Verilator: Sim-Accel Backend-Neutral Program
//
// Data structures shared between lowering and backend emission.
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

#ifndef VERILATOR_V3SIMACCELPROGRAM_H_
#define VERILATOR_V3SIMACCELPROGRAM_H_

#include "config_build.h"
#include "verilatedos.h"

#include <cstddef>
#include <cstdint>
#include <vector>

class V3SimAccelProgram final {
public:
    static constexpr size_t INVALID_SLOT = static_cast<size_t>(-1);

    enum class ExprKind : uint8_t {
        VAR,
        CONST,
        AND,
        OR,
        XOR,
        LOGAND,
        LOGOR,
        LOGNOT,
        ADD,
        SUB,
        EQ,
        NEQ,
        COND,
        CONCAT,
        SEL,
        SHIFTL,
        SHIFTLOVR,
        SHIFTR,
        SHIFTROVR,
        NOT,
        CCAST,
        EXTEND,
        EXTENDS,
    };

    struct Expr final {
        ExprKind m_kind = ExprKind::CONST;
        uint32_t m_width = 0;
        size_t m_lhs = static_cast<size_t>(-1);
        size_t m_rhs = static_cast<size_t>(-1);
        size_t m_third = static_cast<size_t>(-1);
        size_t m_varIdx = static_cast<size_t>(-1);
        uint32_t m_constValue = 0;
    };

    struct Var final {
        string m_name;
        string m_hierarchy;
        string m_direction;
        uint32_t m_width = 0;
        bool m_isPrimaryIo = false;
        bool m_isActivator = false;
        bool m_isCpuVisible = false;
        bool m_isGpuInput = false;
        bool m_isGpuOutput = false;
        size_t m_inputSlot = INVALID_SLOT;
        size_t m_outputSlot = INVALID_SLOT;
    };

    struct Assign final {
        size_t m_lhsIdx = 0;
        string m_lhsName;
        size_t m_exprIdx = 0;
        std::vector<size_t> m_rhsIdxs;
    };

    struct Stats final {
        size_t m_assignwSupported = 0;
        size_t m_assignwTotal = 0;
        size_t m_assignwIgnored = 0;
    };

    struct CommPlan final {
        std::vector<size_t> m_cpuToGpuVarIdxs;
        std::vector<size_t> m_gpuToCpuVarIdxs;
        std::vector<size_t> m_cpuVisibleVarIdxs;
    };

    std::vector<Expr> m_exprs;
    std::vector<Var> m_vars;
    std::vector<Assign> m_assigns;
    Stats m_stats;
    CommPlan m_commPlan;
};

#endif  // Guard
