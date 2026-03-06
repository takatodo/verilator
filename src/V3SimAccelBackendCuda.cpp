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
#include "V3Options.h"
#include "V3SimAccelBackendCudaWriter.h"
#include "V3SimAccelLowerAssignwBool32.h"
#include "V3SimAccelProgram.h"

#include <iomanip>
#include <sstream>

namespace {

void emitStats(const V3SimAccelProgram& program, size_t emittedUniqueAssignw) {
    const size_t supportedAssignw = program.m_stats.m_assignwSupported;
    const size_t totalAssignw = program.m_stats.m_assignwTotal;
    const size_t skipped = program.m_stats.m_assignwIgnored;
    if (skipped) {
        v3info("--sim-accel-only ignored " << skipped
                                          << " ASSIGNW nodes (unsupported or internal constructs)");
    }
    const double offloadPct = totalAssignw
                                  ? (100.0 * static_cast<double>(supportedAssignw)
                                     / static_cast<double>(totalAssignw))
                                  : 0.0;
    std::ostringstream offloadStr;
    offloadStr << std::fixed << std::setprecision(2) << offloadPct;
    v3info("--sim-accel-only stats "
           << "assignw_supported=" << supportedAssignw << " "
           << "assignw_total=" << totalAssignw << " "
           << "assignw_ignored=" << skipped << " "
           << "assignw_emitted_unique=" << emittedUniqueAssignw << " "
           << "assignw_offload_pct=" << offloadStr.str());
}

}  // namespace

void V3SimAccelBackendCuda::emitCuda() VL_MT_DISABLED {
    const string strategy = v3Global.opt.simAccelStrategy();
    if (strategy != "assignw-bool32") {
        v3fatal("Unsupported sim-accel CUDA strategy: " + strategy);  // LCOV_EXCL_LINE
    }
    const string filename = (v3Global.opt.simAccelOutput().empty()
                                 ? v3Global.opt.makeDir() + "/" + v3Global.opt.prefix()
                                       + ".sim_accel.kernel.cu"
                                 : v3Global.opt.simAccelOutput());
    if (v3Global.opt.simAccelSplitModules()) {
        v3fatal("--sim-accel-split-modules is not implemented yet");  // LCOV_EXCL_LINE
    }

    const V3SimAccelProgram program = V3SimAccelLowerAssignwBool32::build();
    if (program.m_assigns.empty()) {
        v3fatal("No supported ASSIGNW nodes found for --sim-accel-only "
                "(supported expr ops: VARREF/CONST/AND/OR/XOR/LOGAND/LOGOR/LOGNOT/NOT/ADD/SUB/"
                "EQ/NEQ/COND/CONCAT/SEL/SHIFT plus CCAST/EXTEND wrappers)");
    }

    const size_t emittedUniqueAssignw = V3SimAccelBackendCudaWriter::write(filename, program);
    emitStats(program, emittedUniqueAssignw);
}
