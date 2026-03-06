// DESCRIPTION: Verilator: Sim-Accel CUDA File Writer
//
// Emits CUDA kernels and metadata files from backend-neutral sim-accel programs.
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

#ifndef VERILATOR_V3SIMACCELBACKENDCUDAWRITER_H_
#define VERILATOR_V3SIMACCELBACKENDCUDAWRITER_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3SimAccelProgram.h"

class V3SimAccelBackendCudaWriter final {
public:
    static size_t write(const string& filename, const V3SimAccelProgram& program);

private:
    V3SimAccelBackendCudaWriter() = default;
    ~V3SimAccelBackendCudaWriter() = default;
};

#endif  // Guard
