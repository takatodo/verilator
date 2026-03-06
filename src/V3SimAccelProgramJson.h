// DESCRIPTION: Verilator: Sim-Accel Program JSON Writer
//
// Serializes backend-neutral sim-accel programs to a stable JSON format.
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

#ifndef VERILATOR_V3SIMACCELPROGRAMJSON_H_
#define VERILATOR_V3SIMACCELPROGRAMJSON_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3SimAccelProgram.h"

class V3SimAccelProgramJson final {
public:
    static void write(const string& filename, const V3SimAccelProgram& program,
                      const string& strategy);

private:
    V3SimAccelProgramJson() = default;
    ~V3SimAccelProgramJson() = default;
};

#endif  // Guard
