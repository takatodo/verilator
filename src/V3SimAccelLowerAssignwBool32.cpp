// DESCRIPTION: Verilator: Sim-Accel assignw-bool32 Lowering
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

#include "V3SimAccelLowerAssignwBool32.h"

#include "V3Global.h"
#include "V3SimAccelLowerAssignwBool32Visitor.h"

V3SimAccelProgram V3SimAccelLowerAssignwBool32::build() {
    return V3SimAccelLowerAssignwBool32Visitor::build(v3Global.rootp());
}
