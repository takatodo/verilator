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

kernel_filename = test.obj_dir + "/renamed-" + test.name + ".sim_accel.kernel.cu"
vars_filename = kernel_filename + ".vars.tsv"
comm_filename = kernel_filename + ".comm.tsv"
kernel_api = kernel_filename + ".api.h"
kernel_cpu = kernel_filename + ".cpu.cpp"

test.compile(verilator_flags2=[
    "--no-std",
    "--sim-accel-only",
    "--sim-accel-output", kernel_filename,
    "--sim-accel-strategy", "assignw-bool32",
    "--top-module", "t",
],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False)

test.file_grep(kernel_filename, r"sim_accel_eval_input_count")
test.file_grep(kernel_filename, r"sim_accel_eval_output_count")
test.file_grep(kernel_filename, r"sim_accel_eval_input_var_index")
test.file_grep(kernel_filename, r"sim_accel_eval_output_var_index")
test.file_grep(vars_filename, r"is_cpu_visible\tis_gpu_input\tis_gpu_output\tinput_slot\toutput_slot")
test.file_grep(comm_filename, r"direction\tslot\tvar_idx\tname\twidth\tis_cpu_visible")
test.file_grep(comm_filename, r"cpu_to_gpu\t0\t[0-9]+\ta\t1\t1")
test.file_grep(comm_filename, r"cpu_to_gpu\t1\t[0-9]+\tb\t1\t1")
test.file_grep(comm_filename, r"gpu_to_cpu\t[0-9]+\t[0-9]+\ty\t1\t1")
test.file_grep_not(comm_filename, r"gpu_to_cpu\t[0-9]+\t[0-9]+\ttmp\t1\t0")

for filename in glob.glob(test.obj_dir + "/*"):
    if filename in [kernel_api, kernel_cpu]:
        continue
    if re.search(r'\.(cpp|cc|cxx|h|hpp)$', filename):
        test.error("%Error: Created '" + filename
                   + "', but --sim-accel-only shouldn't create C++ model files")

test.passes()
