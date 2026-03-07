#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import json
import os
import shutil
import tempfile

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
bench_dir_skip = test.obj_dir + "/sim_accel_bench_skip_cpu_ref"
bench_dir_hybrid = test.obj_dir + "/sim_accel_bench_hybrid"
bench_dir_hybrid_cluster = test.obj_dir + "/sim_accel_bench_hybrid_cluster"
bench_dir_hybrid_cluster_auto = test.obj_dir + "/sim_accel_bench_hybrid_cluster_auto"
bench_dir_program_hex = test.obj_dir + "/sim_accel_bench_program_hex"
bench_dir_memory_image = test.obj_dir + "/sim_accel_bench_memory_image"
bench_run_log = bench_dir_hit + "/bench_run.log"
bench_objhit_log = bench_dir_objhit + "/bench_run.log"
bench_skip_log = bench_dir_skip + "/bench_run.log"
bench_hybrid_log = bench_dir_hybrid + "/bench_run.log"
bench_hybrid_cluster_log = bench_dir_hybrid_cluster + "/bench_run.log"
bench_hybrid_cluster_auto_log = bench_dir_hybrid_cluster_auto + "/bench_run.log"
bench_program_hex_log = bench_dir_program_hex + "/bench_run.log"
bench_memory_image_log = bench_dir_memory_image + "/bench_run.log"
kernel_log = bench_dir_hit + "/verilator_cuda.log"
kernel_cu = bench_dir_hit + "/t.sim_accel.kernel.cu"
kernel_vars = kernel_cu + ".vars.tsv"
kernel_deps = kernel_cu + ".deps.tsv"
kernel_comm = kernel_cu + ".comm.tsv"
kernel_api = kernel_cu + ".api.h"
kernel_cpu = kernel_cu + ".cpu.cpp"
kernel_link = kernel_cu + ".link.cu"
kernel_parts = kernel_cu + ".partitions.tsv"

for path in [cache_dir, bench_dir_miss, bench_dir_hit, bench_dir_objhit, bench_dir_hybrid,
             bench_dir_skip, bench_dir_hybrid_cluster, bench_dir_hybrid_cluster_auto,
             bench_dir_program_hex, bench_dir_memory_image]:
    shutil.rmtree(path, ignore_errors=True)

program_hex_dir = tempfile.mkdtemp(prefix="sim_accel_bench_program_hex_", dir=test.obj_dir)
program_hex_path = program_hex_dir + "/program.hex"
program_map_path = program_hex_dir + "/program.map"
program_target_path = program_hex_dir + "/program.target.json"
memory_image_path = program_hex_dir + "/memory.bin"
memory_map_path = program_hex_dir + "/memory.map"
memory_target_path = program_hex_dir + "/memory.target.json"
with open(program_hex_path, "w", encoding="utf-8") as fh:
    fh.write("@10000000\n")
    fh.write("01 00 00 00\n")
    fh.write("02 00 00 00\n")
    fh.write("03 00 00 00\n")
    fh.write("04 00 00 00\n")
with open(program_map_path, "w", encoding="utf-8") as fh:
    fh.write("0x10000000 a\n")
    fh.write("0x10000004 b\n")
    fh.write("0x10000008 c\n")
    fh.write("0x1000000C d\n")
with open(program_target_path, "w", encoding="utf-8") as fh:
    json.dump(
        {
            "kind": "mapped-visible-preload-v1",
            "name": "program_hex_target",
            "map_file": program_map_path,
            "default_format": "hex",
        },
        fh,
    )
with open(memory_image_path, "wb") as fh:
    fh.write(bytes([0x0A, 0x00, 0x00, 0x00,
                    0x02, 0x00, 0x00, 0x00,
                    0x03, 0x00, 0x00, 0x00,
                    0x04, 0x00, 0x00, 0x00]))
with open(memory_map_path, "w", encoding="utf-8") as fh:
    fh.write("0x0 a\n")
    fh.write("0x4 b\n")
    fh.write("0x8 c\n")
    fh.write("0xC d\n")
with open(memory_target_path, "w", encoding="utf-8") as fh:
    json.dump(
        {
            "kind": "memory-array-preload-v1",
            "name": "memory_image_target",
            "target_path": "t.mem",
            "word_bits": 32,
            "depth": 4,
            "base_addr": 0,
            "address_unit_bytes": 4,
            "default_format": "bin",
            "endianness": "little",
            "elements": [
                {"var_name": "a", "offset": 0, "byte_count": 4, "index": 0},
                {"var_name": "b", "offset": 4, "byte_count": 4, "index": 1},
                {"var_name": "c", "offset": 8, "byte_count": 4, "index": 2},
                {"var_name": "d", "offset": 12, "byte_count": 4, "index": 3},
            ],
        },
        fh,
    )

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
bench_cmd_skip = (
    os.environ["VERILATOR_ROOT"] + "/bin/verilator --sim-accel-bench"
    + " --top-module t"
    + " --nstates 2048"
    + " --gpu-reps 4"
    + " --cpu-reps 2"
    + " --assigns-per-kernel 2"
    + " --compile-cache-dir " + cache_dir
    + " --skip-cpu-reference-build"
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
    + " --hybrid-partition-index 1"
    + " --compile-cache-dir " + cache_dir
    + " -- "
    + test.t_dir + "/t_sim_accel_bench_exec.v")
bench_cmd_hybrid_cluster = (
    os.environ["VERILATOR_ROOT"] + "/bin/verilator --sim-accel-bench"
    + " --top-module t"
    + " --nstates 2048"
    + " --gpu-reps 4"
    + " --cpu-reps 2"
    + " --assigns-per-kernel 2"
    + " --hybrid-mode single-cluster"
    + " --hybrid-cluster-index 0"
    + " --compile-cache-dir " + cache_dir
    + " -- "
    + test.t_dir + "/t_sim_accel_bench_exec.v")
bench_cmd_hybrid_cluster_auto = (
    os.environ["VERILATOR_ROOT"] + "/bin/verilator --sim-accel-bench"
    + " --top-module t"
    + " --nstates 2048"
    + " --gpu-reps 4"
    + " --cpu-reps 2"
    + " --assigns-per-kernel 2"
    + " --hybrid-mode single-cluster"
    + " --hybrid-cluster-auto"
    + " --hybrid-cluster-auto-max-input-bits 128"
    + " --compile-cache-dir " + cache_dir
    + " -- "
    + test.t_dir + "/t_sim_accel_bench_exec.v")
bench_cmd_program_hex = (
    os.environ["VERILATOR_ROOT"] + "/bin/verilator --sim-accel-bench"
    + " --top-module t"
    + " --nstates 2048"
    + " --gpu-reps 4"
    + " --cpu-reps 2"
    + " --assigns-per-kernel 2"
    + " --compile-cache-dir " + cache_dir
    + " --program-hex " + program_hex_path
    + " --program-hex-target " + program_target_path
    + " --program-hex-iterations 8"
    + " -- "
    + test.t_dir + "/t_sim_accel_bench_exec.v")
bench_cmd_memory_image = (
    os.environ["VERILATOR_ROOT"] + "/bin/verilator --sim-accel-bench"
    + " --top-module t"
    + " --nstates 2048"
    + " --gpu-reps 4"
    + " --cpu-reps 2"
    + " --assigns-per-kernel 2"
    + " --compile-cache-dir " + cache_dir
    + " --memory-image " + memory_image_path
    + " --memory-image-target " + memory_target_path
    + " --memory-image-format bin"
    + " -- "
    + test.t_dir + "/t_sim_accel_bench_exec.v")

test.run_capture(bench_cmd.replace("-- ", "--outdir " + bench_dir_miss + " -- ", 1))
test.run_capture(bench_cmd_hit.replace("-- ", "--outdir " + bench_dir_hit + " -- ", 1))
test.run_capture(bench_cmd_object_hit.replace("-- ", "--outdir " + bench_dir_objhit + " -- ", 1))
test.run_capture(bench_cmd_skip.replace("-- ", "--outdir " + bench_dir_skip + " -- ", 1))
test.run_capture(bench_cmd_hybrid.replace("-- ", "--outdir " + bench_dir_hybrid + " -- ", 1))
test.run_capture(
    bench_cmd_hybrid_cluster.replace("-- ", "--outdir " + bench_dir_hybrid_cluster + " -- ", 1))
test.run_capture(
    bench_cmd_hybrid_cluster_auto.replace("-- ", "--outdir " + bench_dir_hybrid_cluster_auto + " -- ", 1))
test.run_capture(
    bench_cmd_program_hex.replace("-- ", "--outdir " + bench_dir_program_hex + " -- ", 1))
test.run_capture(
    bench_cmd_memory_image.replace("-- ", "--outdir " + bench_dir_memory_image + " -- ", 1))

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
test.file_grep(bench_skip_log, r"cpu_reference_build=0")
test.file_grep(bench_skip_log, r"cpu_reference_checked=0")
test.file_grep(bench_skip_log, r"cpu_ms_per_rep=0\.000000")
test.file_grep(bench_skip_log, r"speedup_gpu_over_cpu=skipped")
test.file_grep(bench_skip_log, r"mismatch=0")
test.file_grep(bench_skip_log, r"compact_mismatch=0")
test.file_grep(bench_hybrid_log, r"hybrid_mode=single-partition")
test.file_grep(bench_hybrid_log, r"hybrid_partition_index=1")
test.file_grep(bench_hybrid_log, r"hybrid_partition_assign_count=")
test.file_grep(bench_hybrid_log, r"hybrid_partition_read_vars=")
test.file_grep(bench_hybrid_log, r"hybrid_partition_write_vars=")
test.file_grep(bench_hybrid_log, r"hybrid_partition_input_bytes_per_batch=")
test.file_grep(bench_hybrid_log, r"hybrid_partition_output_bytes_per_batch=")
test.file_grep(bench_hybrid_log, r"hybrid_static_h2d_bytes_per_batch=")
test.file_grep(bench_hybrid_log, r"hybrid_dynamic_h2d_bytes_per_batch=")
test.file_grep(bench_hybrid_log, r"hybrid_actual_h2d_bytes_per_batch=[1-9][0-9]*")
test.file_grep(bench_hybrid_log, r"hybrid_actual_d2h_bytes_per_batch=")
test.file_grep(bench_hybrid_log, r"hybrid_setup_h2d_ms=")
test.file_grep(bench_hybrid_log, r"hybrid_dynamic_h2d_ms_per_rep=")
test.file_grep(bench_hybrid_log, r"hybrid_kernel_ms_per_rep=")
test.file_grep(bench_hybrid_log, r"hybrid_d2h_ms_per_rep=")
test.file_grep(bench_hybrid_log, r"hybrid_transfer_ms_per_rep=")
test.file_grep(bench_hybrid_log, r"hybrid_cpu_dispatch_ms_per_rep=")
test.file_grep(bench_hybrid_log, r"hybrid_gpu_ms_per_rep=")
test.file_grep(bench_hybrid_log, r"hybrid_cpu_ms_per_rep=")
test.file_grep(bench_hybrid_log, r"hybrid_mismatch=0")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_mode=single-cluster")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_cluster_index=0")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_cluster_assign_count=")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_cluster_input_signature_bits=")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_cluster_owner_hint=")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_cluster_read_vars=")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_cluster_write_vars=")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_cluster_input_bytes_per_batch=")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_cluster_output_bytes_per_batch=")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_static_h2d_bytes_per_batch=")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_dynamic_h2d_bytes_per_batch=")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_setup_h2d_ms=")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_dynamic_h2d_ms_per_rep=")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_kernel_ms_per_rep=")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_d2h_ms_per_rep=")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_transfer_ms_per_rep=")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_cpu_dispatch_ms_per_rep=")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_gpu_ms_per_rep=")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_cpu_ms_per_rep=")
test.file_grep(bench_hybrid_cluster_log, r"hybrid_mismatch=0")
test.file_grep(bench_hybrid_cluster_auto_log, r"hybrid_mode=single-cluster")
test.file_grep(bench_hybrid_cluster_auto_log, r"hybrid_cluster_auto=1")
test.file_grep(bench_hybrid_cluster_auto_log, r"hybrid_cluster_auto_candidate_count=[1-9][0-9]*")
test.file_grep(bench_hybrid_cluster_auto_log, r"hybrid_cluster_auto_selected_owner_hint=cpu_boundary_heavy")
test.file_grep(bench_hybrid_cluster_auto_log, r"hybrid_cluster_auto_fallback_used=1")
test.file_grep(bench_hybrid_cluster_auto_log, r"hybrid_static_h2d_bytes_per_batch=[1-9][0-9]*")
test.file_grep(bench_hybrid_cluster_auto_log, r"hybrid_dynamic_h2d_bytes_per_batch=0")
test.file_grep(bench_hybrid_cluster_auto_log, r"hybrid_actual_h2d_bytes_per_batch=[1-9][0-9]*")
test.file_grep(bench_hybrid_cluster_auto_log, r"hybrid_setup_h2d_ms=")
test.file_grep(bench_hybrid_cluster_auto_log, r"hybrid_dynamic_h2d_ms_per_rep=")
test.file_grep(bench_hybrid_cluster_auto_log, r"hybrid_kernel_ms_per_rep=")
test.file_grep(bench_hybrid_cluster_auto_log, r"hybrid_d2h_ms_per_rep=")
test.file_grep(bench_hybrid_cluster_auto_log, r"hybrid_transfer_ms_per_rep=")
test.file_grep(bench_hybrid_cluster_auto_log, r"hybrid_cpu_dispatch_ms_per_rep=")
test.file_grep(bench_hybrid_cluster_auto_log, r"hybrid_mismatch=0")
test.file_grep(bench_program_hex_log, r"program_hex=.*program\.hex")
test.file_grep(bench_program_hex_log, r"program_hex_target=.*program\.target\.json")
test.file_grep(bench_program_hex_log, r"program_hex_iterations=8")
test.file_grep(bench_program_hex_log, r"program_hex_materialized=.*program_hex\.materialized")
test.file_grep(bench_program_hex_log, r"program_hex_init_file=.*program_hex\.init")
test.file_grep(bench_program_hex_log, r"program_hex_preload_tsv=.*program_hex\.preload\.tsv")
test.file_grep(bench_program_hex_log, r"program_hex_preload_entries=4")
test.file_grep(bench_program_hex_log, r"program_hex_direct_file=.*program_hex\.direct\.tsv")
test.file_grep(bench_program_hex_log, r"program_hex_direct_entries=4")
test.file_grep(bench_program_hex_log, r"direct_preload_file_count=1")
test.file_grep(bench_program_hex_log, r"direct_preload_rules_applied=4")
test.file_grep(bench_program_hex_log, r"direct_preload_values_applied=8192")
test.file_grep(bench_program_hex_log, r"mismatch=0")
test.file_grep(bench_dir_program_hex + r"/program_hex.materialized", r"08 00 00 00")
test.file_grep(bench_dir_program_hex + r"/program_hex.init", r"^a 0x00000008$")
test.file_grep(bench_dir_program_hex + r"/program_hex.init", r"^b 0x00000002$")
test.file_grep(bench_dir_program_hex + r"/program_hex.init", r"^c 0x00000003$")
test.file_grep(bench_dir_program_hex + r"/program_hex.init", r"^d 0x00000004$")
test.file_grep(bench_memory_image_log, r"memory_image=.*memory\.bin")
test.file_grep(bench_memory_image_log, r"memory_image_target=.*memory\.target\.json")
test.file_grep(bench_memory_image_log, r"memory_image_format=bin")
test.file_grep(bench_memory_image_log, r"memory_image_materialized=.*memory_image\.materialized\.bin")
test.file_grep(bench_memory_image_log, r"memory_image_init_file=.*memory_image\.init")
test.file_grep(bench_memory_image_log, r"memory_image_preload_tsv=.*memory_image\.preload\.tsv")
test.file_grep(bench_memory_image_log, r"memory_image_preload_entries=4")
test.file_grep(bench_memory_image_log, r"memory_image_direct_file=.*memory_image\.direct\.tsv")
test.file_grep(bench_memory_image_log, r"memory_image_direct_entries=4")
test.file_grep(bench_memory_image_log, r"memory_image_payload_tsv=.*memory_image\.payload\.tsv")
test.file_grep(bench_memory_image_log, r"memory_image_payload_entries=4")
test.file_grep(bench_memory_image_log, r"array_preload_payload_file_count=1")
test.file_grep(bench_memory_image_log, r"array_preload_payload_files_loaded=1")
test.file_grep(bench_memory_image_log, r"array_preload_targets_loaded=1")
test.file_grep(bench_memory_image_log, r"array_preload_words_loaded=4")
test.file_grep(bench_memory_image_log, r"array_preload_mapped_rows_loaded=4")
test.file_grep(bench_memory_image_log, r"array_preload_mapped_rules_applied=4")
test.file_grep(bench_memory_image_log, r"array_preload_mapped_values_applied=8192")
test.file_grep(bench_memory_image_log, r"array_preload_lines_ignored=0")
test.file_grep(bench_memory_image_log, r"direct_preload_file_count=1")
test.file_grep(bench_memory_image_log, r"direct_preload_rules_applied=4")
test.file_grep(bench_memory_image_log, r"direct_preload_values_applied=8192")
test.file_grep(bench_memory_image_log, r"mismatch=0")
test.file_grep(bench_dir_memory_image + r"/memory_image.init", r"^a 0x0000000A$")
test.file_grep(bench_dir_memory_image + r"/memory_image.init", r"^b 0x00000002$")
test.file_grep(bench_dir_memory_image + r"/memory_image.init", r"^c 0x00000003$")
test.file_grep(bench_dir_memory_image + r"/memory_image.init", r"^d 0x00000004$")
test.file_grep(
    bench_dir_memory_image + r"/memory_image.payload.tsv",
    r"^target_path\tword_index\tvalue_hex\tword_bits\tbase_addr\taddress_unit_bytes\tendianness\tvar_name\tvar_index\twidth\tvisible$")
test.file_grep(
    bench_dir_memory_image + r"/memory_image.payload.tsv",
    r"^t\.mem\t0\t0x0000000A\t32\t0x00000000\t4\tlittle\ta\t3\t32\t1$")
test.file_grep(
    bench_dir_memory_image + r"/memory_image.payload.tsv",
    r"^t\.mem\t3\t0x00000004\t32\t0x00000000\t4\tlittle\td\t2\t32\t1$")
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
test.file_grep(bench_objhit_log, r"object_cache_hits=[1-9][0-9]*")
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
