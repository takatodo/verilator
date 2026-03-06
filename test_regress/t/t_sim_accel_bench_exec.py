#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import os

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

bench_dir = test.obj_dir + "/sim_accel_bench"
bench_run_log = bench_dir + "/bench_run.log"
kernel_log = bench_dir + "/verilator_cuda.log"
kernel_cu = bench_dir + "/t.sim_accel.kernel.cu"
kernel_vars = kernel_cu + ".vars.tsv"
kernel_deps = kernel_cu + ".deps.tsv"
kernel_comm = kernel_cu + ".comm.tsv"

test.run_capture(
    os.environ["VERILATOR_ROOT"] + "/bin/verilator --sim-accel-bench"
    + " --top-module t"
    + " --outdir " + bench_dir
    + " --nstates 8192"
    + " --gpu-reps 16"
    + " --cpu-reps 4"
    + " -- "
    + test.t_dir + "/t_sim_accel_bench_exec.v")

for filename in [bench_run_log, kernel_log, kernel_cu, kernel_vars]:
    if not os.path.exists(filename):
        test.error("Expected output file not found: " + filename)
if os.path.exists(kernel_comm):
    test.file_grep(kernel_comm, r"direction\tslot\tvar_idx\tname\twidth\tis_cpu_visible")

test.file_grep(bench_run_log, r"mismatch=0")
test.file_grep(bench_run_log, r"compact_mismatch=0")
test.file_grep(bench_run_log, r"speedup_gpu_over_cpu=")
test.file_grep(bench_run_log, r"auto_engine_recommendation=")
test.file_grep(bench_run_log, r"comm_input_vars=")
test.file_grep(bench_run_log, r"comm_output_vars=")
test.file_grep(bench_run_log, r"comm_index_bytes_static=")
test.file_grep(bench_run_log, r"comm_total_bytes_per_batch=")
test.file_grep(bench_run_log, r"comm_roundtrip_ratio_pct=")
test.file_grep(bench_run_log, r"comm_verify_full_d2h_bytes=")
test.file_grep(bench_run_log, r"verilator_codegen_s=")
test.file_grep(bench_run_log, r"nvcc_compile_s=")
test.file_grep(bench_run_log, r"bench_run_s=")
test.file_grep(bench_run_log, r"total_elapsed_s=")
if os.path.exists(kernel_deps):
    test.file_grep(bench_run_log, r"cuda_deps_rows=")

test.passes()
