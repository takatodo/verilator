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
test.file_grep(out_filename, r'"comm_buffers": \{')
test.file_grep(out_filename, r'"preload_targets": \[')
test.file_grep(out_filename, r'"approx_regcut_analysis": \{')
test.file_grep(out_filename, r'"spec_frontier_analysis": \{')
test.file_grep(out_filename, r'"hybrid_cluster_analysis": \{')
test.file_grep(out_filename, r'"approx_regcut_clusters": \[')
test.file_grep(out_filename, r'"cluster_count": ')
test.file_grep(out_filename, r'"input_signature_var_count": ')
test.file_grep(out_filename, r'"input_signature_bit_count": ')
test.file_grep(out_filename, r'"operator_count": ')
test.file_grep(out_filename, r'"expr_kind_counts": \{')
test.file_grep(out_filename, r'"boundary_input_var_count": ')
test.file_grep(out_filename, r'"boundary_output_var_count": ')
test.file_grep(out_filename, r'"max_assign_count": ')
test.file_grep(out_filename, r'"gpu_candidate_cluster_count": ')
test.file_grep(out_filename, r'"cluster_topo_order": \[')
test.file_grep(out_filename, r'"candidate_count": ')
test.file_grep(out_filename, r'"clusters_with_candidates": ')
test.file_grep(out_filename, r'"preferred_zero_candidate_count": ')
test.file_grep(out_filename, r'"cluster_idx": 0')
test.file_grep(out_filename, r'"topo_rank": 0')
test.file_grep(out_filename, r'"hybrid_owner_hint": "')
test.file_grep(out_filename, r'"assign_idxs": \[')
test.file_grep(out_filename, r'"boundary_input_var_idxs": \[')
test.file_grep(out_filename, r'"boundary_output_var_idxs": \[')
test.file_grep(out_filename, r'"dependency_cluster_idxs": \[')
test.file_grep(out_filename, r'"cpu_boundary_input_var_idxs": \[')
test.file_grep(out_filename, r'"gpu_internal_output_var_idxs": \[')
test.file_grep(out_filename, r'"spec_frontier_candidate_count": ')
test.file_grep(out_filename, r'"spec_frontier_candidates": \[')
test.file_grep(out_filename, r'"preferred_value": 0')
test.file_grep(out_filename, r'"var_name": "a"')
test.file_grep(out_filename, r'"cpu_to_gpu_var_idxs": \[')
test.file_grep(out_filename, r'"gpu_to_cpu_var_idxs": \[')
test.file_grep(out_filename, r'"cpu_visible_var_idxs": \[')
test.file_grep(out_filename, r'"is_gpu_input": true')
test.file_grep(out_filename, r'"is_gpu_output": true')
test.file_grep(out_filename, r'"lhs_name": "y"')

for filename in glob.glob(test.obj_dir + "/*"):
    if re.search(r'\.(cpp|cc|cxx|h|hpp)$', filename):
        test.error("%Error: Created '" + filename
                   + "', but --sim-accel-ir-only shouldn't create C++ model files")

test.passes()
