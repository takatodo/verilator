// DESCRIPTION: Verilator: Sim-Accel CUDA Expr Emitter
//
// Converts backend-neutral sim-accel expressions into CUDA C expressions.
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

#ifndef VERILATOR_V3SIMACCELBACKENDCUDAEXPREMITTER_H_
#define VERILATOR_V3SIMACCELBACKENDCUDAEXPREMITTER_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3SimAccelProgram.h"

class V3SimAccelBackendCudaExprEmitter final {
    const V3SimAccelProgram& m_program;

    string maskWrap(const string& expr, uint32_t width) const;

public:
    explicit V3SimAccelBackendCudaExprEmitter(const V3SimAccelProgram& program);
    string emit(size_t exprIdx) const;
};

#endif  // Guard
