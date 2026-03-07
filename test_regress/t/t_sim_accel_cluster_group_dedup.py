#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import os
import shutil

import vltest_bootstrap

test.scenarios('vlt')


def require_or_skip(message: str) -> None:
    if os.getenv("VERILATOR_TEST_REQUIRE_SIM_ACCEL_CUDA") or os.getenv("VERILATOR_TEST_REQUIRE_GEM_CUDA"):
        test.error(message)
    else:
        test.skip(message)


if not test.run_capture("nvcc --version", check=False):
    require_or_skip("No nvcc installed")
if not test.run_capture("nvidia-smi -L", check=False):
    require_or_skip("No visible NVIDIA GPU")

cache_dir = test.obj_dir + "/sim_accel_cluster_group_dedup_cache"
bench_dir = test.obj_dir + "/sim_accel_cluster_group_dedup_bench"
bench_log = bench_dir + "/bench_run.log"

for path in [cache_dir, bench_dir]:
    shutil.rmtree(path, ignore_errors=True)

bench_cmd = (
    os.environ["VERILATOR_ROOT"] + "/bin/verilator --sim-accel-bench"
    + " --top-module t"
    + " --outdir " + bench_dir
    + " --nstates 2048"
    + " --gpu-reps 4"
    + " --cpu-reps 2"
    + " --init-mode zero"
    + " --hybrid-mode cluster-group"
    + " --hybrid-cluster-indices 0,1"
    + " --hybrid-batch-dedup"
    + " --compile-cache-dir " + cache_dir
    + " -- "
    + test.t_dir + "/t_sim_accel_cluster_group.v")

test.run_capture(bench_cmd)

test.file_grep(bench_log, r"hybrid_mode=cluster-group")
test.file_grep(bench_log, r"hybrid_cluster_indices=0,1")
test.file_grep(bench_log, r"hybrid_batch_dedup=1")
test.file_grep(bench_log, r"hybrid_dedup_boundary_vars=[1-9][0-9]*")
test.file_grep(bench_log, r"hybrid_dedup_unique_states_avg=1\.000000")
test.file_grep(bench_log, r"hybrid_dedup_unique_ratio_pct=0\.048828")
test.file_grep(bench_log, r"hybrid_dedup_effective_h2d_bytes_per_rep=[1-9][0-9]*")
test.file_grep(bench_log, r"hybrid_dedup_effective_d2h_bytes_per_rep=[1-9][0-9]*")
test.file_grep(bench_log, r"hybrid_mismatch=0")

test.passes()
