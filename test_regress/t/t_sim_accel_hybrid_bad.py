#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import os
import subprocess

import vltest_bootstrap

test.scenarios('vlt')

def expect_failure(extra_args, expected_text):
    cmd = [
        os.environ["VERILATOR_ROOT"] + "/bin/verilator",
        "--sim-accel-bench",
        "--top-module", "t",
        "--outdir", test.obj_dir + "/sim_accel_hybrid_bad",
        "--nstates", "16",
        "--gpu-reps", "1",
        "--cpu-reps", "1",
        *extra_args,
        "--",
        test.t_dir + "/t_sim_accel_bench_exec.v",
    ]
    if test.verbose:
        print("\t" + " ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    combined = proc.stdout + proc.stderr
    if proc.returncode == 0:
        test.error("Expected hybrid bad bench invocation to fail")
    if expected_text not in combined:
        test.error("Expected error text not found.\nWanted: " + expected_text + "\nGot:\n" + combined)


expect_failure(["--hybrid-mode", "single-partition"],
               "--hybrid-partition-index is required with --hybrid-mode single-partition")
expect_failure(["--hybrid-mode", "single-cluster"],
               "--hybrid-cluster-index is required with --hybrid-mode single-cluster")

test.passes()
