#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import glob
import re

import vltest_bootstrap

test.scenarios('vlt')

out_filename = test.obj_dir + "/renamed-" + test.name + ".sim_accel.program.json"

test.compile(verilator_flags2=[
    "--no-std",
    "--sim-accel-ir-only",
    "--sim-accel-ir-output", out_filename,
    "--sim-accel-strategy", "assignw-bool32",
    "--top-module", "t",
],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False)

test.file_grep(out_filename, r'"format": "sim-accel-program-v1"')
test.file_grep(out_filename, r'"strategy": "assignw-bool32"')
test.file_grep(out_filename, r'"vars": \[')
test.file_grep(out_filename, r'"exprs": \[')
test.file_grep(out_filename, r'"assigns": \[')
test.file_grep(out_filename, r'"lhs_name": "y"')

for filename in glob.glob(test.obj_dir + "/*"):
    if re.search(r'\.(cpp|cc|cxx|h|hpp)$', filename):
        test.error("%Error: Created '" + filename
                   + "', but --sim-accel-ir-only shouldn't create C++ model files")

test.passes()
