// DESCRIPTION: Verilator: GEM Back-end Emitter
//
// This file is responsible for emitting GEM (GPU-Accelerated Emulator-Inspired
// RTL Simulation) intermediate representation and CUDA kernels.
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
