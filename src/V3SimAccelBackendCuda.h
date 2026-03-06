// DESCRIPTION: Verilator: Sim-Accel CUDA Backend
//
// Selects the active CUDA lowering strategy and emits backend-specific output.
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

#ifndef VERILATOR_V3SIMACCELBACKENDCUDA_H_
#define VERILATOR_V3SIMACCELBACKENDCUDA_H_

#include "config_build.h"
#include "verilatedos.h"

class V3SimAccelBackendCuda final {
public:
    static void emitIr();
    static void emitCuda();

private:
    V3SimAccelBackendCuda() = default;
    ~V3SimAccelBackendCuda() = default;
};

#endif  // Guard
