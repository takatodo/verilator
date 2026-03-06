// DESCRIPTION: Verilator: Sim-Accel assignw-bool32 AST Visitor
//
// Internal AST visitor used to lower the supported ASSIGNW-based bool32 subset
// into the backend-neutral sim-accel program representation.
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

#ifndef VERILATOR_V3SIMACCELLOWERASSIGNWBOOL32VISITOR_H_
#define VERILATOR_V3SIMACCELLOWERASSIGNWBOOL32VISITOR_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3SimAccelProgram.h"

class AstNetlist;

class V3SimAccelLowerAssignwBool32Visitor final {
public:
    static V3SimAccelProgram build(AstNetlist* rootp);

private:
    V3SimAccelLowerAssignwBool32Visitor() = default;
    ~V3SimAccelLowerAssignwBool32Visitor() = default;
};

#endif  // Guard
