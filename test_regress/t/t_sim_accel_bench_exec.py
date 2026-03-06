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

cache_dir = test.obj_dir + "/sim_accel_cache"
bench_dir_miss = test.obj_dir + "/sim_accel_bench_miss"
bench_dir_hit = test.obj_dir + "/sim_accel_bench_hit"
bench_dir_objhit = test.obj_dir + "/sim_accel_bench_object_hit"
bench_dir_hybrid = test.obj_dir + "/sim_accel_bench_hybrid"
bench_run_log = bench_dir_hit + "/bench_run.log"
bench_objhit_log = bench_dir_objhit + "/bench_run.log"
bench_hybrid_log = bench_dir_hybrid + "/bench_run.log"
kernel_log = bench_dir_hit + "/verilator_cuda.log"
kernel_cu = bench_dir_hit + "/t.sim_accel.kernel.cu"
kernel_vars = kernel_cu + ".vars.tsv"
kernel_deps = kernel_cu + ".deps.tsv"
kernel_comm = kernel_cu + ".comm.tsv"
kernel_api = kernel_cu + ".api.h"
kernel_cpu = kernel_cu + ".cpu.cpp"
kernel_link = kernel_cu + ".link.cu"
kernel_parts = kernel_cu + ".partitions.tsv"

for path in [cache_dir, bench_dir_miss, bench_dir_hit, bench_dir_objhit, bench_dir_hybrid]:
    shutil.rmtree(path, ignore_errors=True)

bench_cmd = (
    os.environ["VERILATOR_ROOT"] + "/bin/verilator --sim-accel-bench"
    + " --top-module t"
    + " --nstates 8192"
    + " --gpu-reps 16"
    + " --cpu-reps 4"
    + " --assigns-per-kernel 2"
    + " --compile-cache-dir " + cache_dir
    + " -- "
    + test.t_dir + "/t_sim_accel_bench_exec.v")
bench_cmd_hit = (
    os.environ["VERILATOR_ROOT"] + "/bin/verilator --sim-accel-bench"
    + " --top-module t"
    + " --nstates 8192"
    + " --gpu-reps 8"
    + " --cpu-reps 2"
    + " --assigns-per-kernel 2"
    + " --compile-cache-dir " + cache_dir
    + " -- "
    + test.t_dir + "/t_sim_accel_bench_exec.v")
bench_cmd_object_hit = (
    os.environ["VERILATOR_ROOT"] + "/bin/verilator --sim-accel-bench"
    + " --top-module t"
    + " --nstates 4096"
    + " --gpu-reps 4"
    + " --cpu-reps 1"
    + " --assigns-per-kernel 2"
    + " --compile-cache-dir " + cache_dir
    + " --no-compile-cache"
    + " -- "
    + test.t_dir + "/t_sim_accel_bench_exec.v")
bench_cmd_hybrid = (
    os.environ["VERILATOR_ROOT"] + "/bin/verilator --sim-accel-bench"
    + " --top-module t"
    + " --nstates 2048"
    + " --gpu-reps 4"
    + " --cpu-reps 2"
    + " --assigns-per-kernel 2"
    + " --hybrid-mode single-partition"
    + " --hybrid-partition-index 0"
    + " --compile-cache-dir " + cache_dir
    + " -- "
    + test.t_dir + "/t_sim_accel_bench_exec.v")

test.run_capture(bench_cmd.replace("-- ", "--outdir " + bench_dir_miss + " -- ", 1))
test.run_capture(bench_cmd_hit.replace("-- ", "--outdir " + bench_dir_hit + " -- ", 1))
test.run_capture(bench_cmd_object_hit.replace("-- ", "--outdir " + bench_dir_objhit + " -- ", 1))
test.run_capture(bench_cmd_hybrid.replace("-- ", "--outdir " + bench_dir_hybrid + " -- ", 1))

for filename in [bench_run_log, kernel_log, kernel_cu, kernel_vars, kernel_api, kernel_cpu, kernel_link, kernel_parts]:
    if not os.path.exists(filename):
        test.error("Expected output file not found: " + filename)
test.file_grep(kernel_parts, r"dominant_hierarchy")
test.file_grep(kernel_parts, r"dominant_hierarchy_key")
test.file_grep(kernel_parts, r"canonical_hash")
test.file_grep(kernel_parts, r"canonical_var_count")
if os.path.exists(kernel_comm):
    test.file_grep(kernel_comm, r"direction\tslot\tvar_idx\tname\twidth\tis_cpu_visible")

test.file_grep(bench_dir_miss + "/bench_run.log", r"nvcc_cache_mode=miss")
test.file_grep(bench_dir_miss + "/bench_run.log", r"verilator_artifact_cache_mode=miss")
test.file_grep(bench_run_log, r"nvcc_cache_mode=hit")
test.file_grep(bench_run_log, r"verilator_artifact_cache_mode=hit")
test.file_grep(bench_objhit_log, r"verilator_artifact_cache_mode=disabled")
test.file_grep(bench_objhit_log, r"nvcc_cache_mode=disabled")
test.file_grep(bench_run_log, r"mismatch=0")
test.file_grep(bench_run_log, r"compact_mismatch=0")
test.file_grep(bench_objhit_log, r"mismatch=0")
test.file_grep(bench_objhit_log, r"compact_mismatch=0")
test.file_grep(bench_hybrid_log, r"hybrid_mode=single-partition")
test.file_grep(bench_hybrid_log, r"hybrid_partition_index=0")
test.file_grep(bench_hybrid_log, r"hybrid_partition_assign_count=")
test.file_grep(bench_hybrid_log, r"hybrid_partition_read_vars=")
test.file_grep(bench_hybrid_log, r"hybrid_partition_write_vars=")
test.file_grep(bench_hybrid_log, r"hybrid_partition_input_bytes_per_batch=")
test.file_grep(bench_hybrid_log, r"hybrid_partition_output_bytes_per_batch=")
test.file_grep(bench_hybrid_log, r"hybrid_actual_h2d_bytes_per_batch=")
test.file_grep(bench_hybrid_log, r"hybrid_actual_d2h_bytes_per_batch=")
test.file_grep(bench_hybrid_log, r"hybrid_gpu_ms_per_rep=")
test.file_grep(bench_hybrid_log, r"hybrid_cpu_ms_per_rep=")
test.file_grep(bench_hybrid_log, r"hybrid_mismatch=0")
test.file_grep(bench_run_log, r"speedup_gpu_over_cpu=")
test.file_grep(bench_run_log, r"kernel_partitions=[2-9][0-9]*")
test.file_grep(bench_run_log, r"auto_engine_recommendation=")
test.file_grep(bench_run_log, r"comm_input_vars=")
test.file_grep(bench_run_log, r"comm_output_vars=")
test.file_grep(bench_run_log, r"comm_index_bytes_static=")
test.file_grep(bench_run_log, r"comm_total_bytes_per_batch=")
test.file_grep(bench_run_log, r"comm_roundtrip_ratio_pct=")
test.file_grep(bench_run_log, r"comm_verify_full_d2h_bytes=")
test.file_grep(bench_run_log, r"cuda_assignw_offload_scope=post_lowering_assignw_nodes")
test.file_grep(bench_run_log, r"cuda_assignw_offload_basis=supported_assignw_div_total_assignw")
test.file_grep(bench_run_log, r"approx_regcut_cluster_count=")
test.file_grep(bench_run_log, r"approx_regcut_assign_count=")
test.file_grep(bench_run_log, r"approx_regcut_boundary_input_vars=")
test.file_grep(bench_run_log, r"approx_regcut_boundary_output_vars=")
test.file_grep(bench_run_log, r"approx_regcut_max_assigns=")
test.file_grep(bench_run_log, r"compile_cache_dir=")
test.file_grep(bench_run_log, r"partition_object_build=1")
test.file_grep(bench_run_log, r"object_cache_hits=")
test.file_grep(bench_run_log, r"object_cache_misses=")
test.file_grep(bench_run_log, r"partition_canonical_total=")
test.file_grep(bench_run_log, r"partition_canonical_unique=")
test.file_grep(bench_run_log, r"partition_canonical_duplicates=")
test.file_grep(bench_run_log, r"partition_canonical_max_reuse=")
test.file_grep(bench_objhit_log, r"partition_object_build=1")
test.file_grep(bench_objhit_log, r"object_cache_hits=5")
test.file_grep(bench_objhit_log, r"object_cache_misses=0")
test.file_grep(bench_run_log, r"verilator_artifact_cache_key=")
test.file_grep(bench_run_log, r"nvcc_cache_key=")
test.file_grep(bench_run_log, r"verilator_codegen_s=0\.000000")
test.file_grep(bench_run_log, r"verilator_codegen_cold_s=")
test.file_grep(bench_run_log, r"nvcc_compile_s=")
test.file_grep(bench_run_log, r"nvcc_cold_compile_s=")
test.file_grep(bench_run_log, r"bench_run_s=")
test.file_grep(bench_run_log, r"total_elapsed_s=")
test.file_grep(bench_run_log, r"total_elapsed_cold_s=")
if os.path.exists(kernel_deps):
    test.file_grep(bench_run_log, r"cuda_deps_rows=")

test.passes()
