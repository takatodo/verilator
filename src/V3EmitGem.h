// DESCRIPTION: Verilator: GEM Back-end Emitter
//
// This file is responsible for emitting GEM (GPU-Accelerated Emulator-Inspired
// RTL Simulation) intermediate representation and CUDA kernels.
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

#ifndef VERILATOR_V3EMITGEM_H_
#define VERILATOR_V3EMITGEM_H_

#include "config_build.h"
#include "verilatedos.h"

//=============================================================================

class V3EmitGem final {
public:
    static void emitGemIr();
    static void emitGemCuda();

private:
    V3EmitGem() = default;
    ~V3EmitGem() = default;
};

#endif  // Guard
