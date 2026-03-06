// DESCRIPTION: Verilator: Sim-Accel Emitter Front Door
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

#include "V3EmitSimAccel.h"

#include "V3Error.h"
#include "V3Global.h"
#include "V3Options.h"
#include "V3SimAccelBackendCuda.h"
#include "V3SimAccelLowerAssignwBool32.h"
#include "V3SimAccelProgram.h"
#include "V3SimAccelProgramJson.h"

void V3EmitSimAccel::emitIr() VL_MT_DISABLED {
    const string filename = (v3Global.opt.simAccelIrOutput().empty()
                                 ? v3Global.opt.makeDir() + "/" + v3Global.opt.prefix()
                                       + ".sim_accel.program.json"
                                 : v3Global.opt.simAccelIrOutput());
    const string strategy = v3Global.opt.simAccelStrategy();
    if (strategy == "assignw-bool32") {
        const V3SimAccelProgram program = V3SimAccelLowerAssignwBool32::build();
        V3SimAccelProgramJson::write(filename, program, strategy);
        return;
    }
    v3fatal("Unsupported sim-accel IR strategy: " + strategy);  // LCOV_EXCL_LINE
}

void V3EmitSimAccel::emitCuda() VL_MT_DISABLED {
    if (v3Global.opt.simAccelBackend() == "cuda") {
        V3SimAccelBackendCuda::emitCuda();
        return;
    }
    v3fatal("Unsupported sim-accel backend: " + v3Global.opt.simAccelBackend());  // LCOV_EXCL_LINE
}
