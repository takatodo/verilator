// DESCRIPTION: Verilator: Sim-Accel Emitter Front Door
//
// Routes sim-accel emission through the selected backend.
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

#ifndef VERILATOR_V3EMITSIMACCEL_H_
#define VERILATOR_V3EMITSIMACCEL_H_

#include "config_build.h"
#include "verilatedos.h"

class V3EmitSimAccel final {
public:
    static void emitIr();
    static void emitCuda();

private:
    V3EmitSimAccel() = default;
    ~V3EmitSimAccel() = default;
};

#endif  // Guard
