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
#include "V3SimAccelStrategyAssignwBool32.h"

void V3SimAccelBackendCuda::emitIr() VL_MT_DISABLED {
    if (v3Global.opt.simAccelStrategy() == "assignw-bool32") {
        V3SimAccelStrategyAssignwBool32::emitIr();
        return;
    }
    v3fatal("Unsupported sim-accel CUDA strategy: "
            + v3Global.opt.simAccelStrategy());  // LCOV_EXCL_LINE
}

void V3SimAccelBackendCuda::emitCuda() VL_MT_DISABLED {
    if (v3Global.opt.simAccelStrategy() == "assignw-bool32") {
        V3SimAccelStrategyAssignwBool32::emitCuda();
        return;
    }
    v3fatal("Unsupported sim-accel CUDA strategy: "
            + v3Global.opt.simAccelStrategy());  // LCOV_EXCL_LINE
}
