#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verifies hidden constant-index array preload execution for sim-accel
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

verilator_root = os.environ["VERILATOR_ROOT"]
target_gen = verilator_root + "/bin/verilator_sim_accel_generate_preload_target"

cache_dir = test.obj_dir + "/sim_accel_hidden_preload_exec_cache"
probe_dir = test.obj_dir + "/sim_accel_hidden_preload_exec_probe"
bench_dir = test.obj_dir + "/sim_accel_hidden_preload_exec_bench"
kernel_path = probe_dir + "/hidden_exec.sim_accel.kernel.cu"
vars_tsv = kernel_path + ".vars.tsv"
preload_targets_tsv = kernel_path + ".preload_targets.tsv"
preload_target_elements_tsv = kernel_path + ".preload_target_elements.tsv"
preload_targets_json = kernel_path + ".preload_targets.json"
target_path = probe_dir + "/hidden_mem.target.json"
bench_log = bench_dir + "/bench_run.log"
memory_init = bench_dir + "/memory_image.init"
memory_payload = bench_dir + "/memory_image.payload.tsv"

for path in [cache_dir, probe_dir, bench_dir]:
    shutil.rmtree(path, ignore_errors=True)
os.makedirs(probe_dir, exist_ok=True)

image_dir = tempfile.mkdtemp(prefix="sim_accel_hidden_preload_exec_", dir=test.obj_dir)
memory_image = image_dir + "/memory.bin"
with open(memory_image, "wb") as fh:
    fh.write(bytes([
        0x0A, 0x00, 0x00, 0x00,
        0x14, 0x00, 0x00, 0x00,
        0x1E, 0x00, 0x00, 0x00,
        0x28, 0x00, 0x00, 0x00,
    ]))

probe_cmd = (
    verilator_root + "/bin/verilator"
    + " --no-std"
    + " --sim-accel-only"
    + " --sim-accel-output " + kernel_path
    + " --top-module t"
    + " " + test.t_dir + "/t_sim_accel_hidden_preload_exec.v")
test.run_capture(probe_cmd)

if not os.path.exists(vars_tsv):
    test.error("Expected vars.tsv not found: " + vars_tsv)
if not os.path.exists(preload_targets_tsv):
    test.error("Expected preload_targets.tsv not found: " + preload_targets_tsv)
if not os.path.exists(preload_target_elements_tsv):
    test.error("Expected preload_target_elements.tsv not found: " + preload_target_elements_tsv)
if not os.path.exists(preload_targets_json):
    test.error("Expected preload_targets.json not found: " + preload_targets_json)

test.file_grep(vars_tsv, r"t__DOT__hidden_mem__BRA__0__KET__")
test.file_grep(vars_tsv, r"t__DOT__hidden_mem__BRA__1__KET__")
test.file_grep(vars_tsv, r"t__DOT__hidden_mem__BRA__2__KET__")
test.file_grep(vars_tsv, r"t__DOT__hidden_mem__BRA__3__KET__")

target_cmd = (
    target_gen
    + " --preload-targets-json " + preload_targets_json
    + " --description 'Generated from hidden array element vars'"
    + " --target-path t.hidden_mem"
    + " --out " + target_path)
test.run_capture(target_cmd)

if not os.path.exists(target_path):
    test.error("Expected generated target not found: " + target_path)

with open(target_path, encoding="utf-8") as fh:
    target_payload = json.load(fh)
if target_payload.get("kind") != "memory-array-preload-v1":
    test.error("Unexpected target kind: " + str(target_payload.get("kind")))
if target_payload.get("target_path") != "t.hidden_mem":
    test.error("Unexpected target path: " + str(target_payload.get("target_path")))
elements = target_payload.get("elements", [])
if len(elements) != 4:
    test.error("Expected 4 generated hidden memory elements, got " + str(len(elements)))

bench_cmd = (
    verilator_root + "/bin/verilator --sim-accel-bench"
    + " --top-module t"
    + " --outdir " + bench_dir
    + " --nstates 2048"
    + " --gpu-reps 4"
    + " --cpu-reps 2"
    + " --compile-cache-dir " + cache_dir
    + " --memory-image " + memory_image
    + " --memory-image-target " + target_path
    + " --memory-image-format bin"
    + " -- "
    + test.t_dir + "/t_sim_accel_hidden_preload_exec.v")
test.run_capture(bench_cmd)

for filename in [bench_log, memory_init, memory_payload]:
    if not os.path.exists(filename):
        test.error("Expected output file not found: " + filename)

test.file_grep(target_path, r'"var_name": "t__DOT__hidden_mem__BRA__0__KET__"')
test.file_grep(target_path, r'"var_name": "t__DOT__hidden_mem__BRA__3__KET__"')
test.file_grep(target_path, r'"offset": 0')
test.file_grep(target_path, r'"offset": 12')
test.file_grep(bench_log, r"memory_image_target=.*hidden_mem\.target\.json")
test.file_grep(bench_log, r"memory_image_preload_entries=4")
test.file_grep(bench_log, r"memory_image_direct_file=.*memory_image\.direct\.tsv")
test.file_grep(bench_log, r"memory_image_direct_entries=4")
test.file_grep(bench_log, r"memory_image_payload_tsv=.*memory_image\.payload\.tsv")
test.file_grep(bench_log, r"memory_image_payload_entries=4")
test.file_grep(bench_log, r"memory_image_payload_visible_entries=4")
test.file_grep(bench_log, r"memory_image_payload_hidden_entries=0")
test.file_grep(bench_log, r"array_preload_payload_file_count=1")
test.file_grep(bench_log, r"array_preload_payload_files_loaded=1")
test.file_grep(bench_log, r"array_preload_targets_loaded=1")
test.file_grep(bench_log, r"array_preload_words_loaded=4")
test.file_grep(bench_log, r"array_preload_mapped_rows_loaded=4")
test.file_grep(bench_log, r"array_preload_hidden_rows_loaded=0")
test.file_grep(bench_log, r"array_preload_hidden_only_targets=0")
test.file_grep(bench_log, r"array_preload_mapped_rules_applied=4")
test.file_grep(bench_log, r"array_preload_mapped_values_applied=8192")
test.file_grep(bench_log, r"array_preload_lines_ignored=0")
test.file_grep(bench_log, r"direct_preload_file_count=1")
test.file_grep(bench_log, r"direct_preload_rules_applied=4")
test.file_grep(bench_log, r"direct_preload_values_applied=8192")
test.file_grep(bench_log, r"mismatch=0")
test.file_grep(memory_init, r"^t__DOT__hidden_mem__BRA__0__KET__ 0x0000000A$")
test.file_grep(memory_init, r"^t__DOT__hidden_mem__BRA__1__KET__ 0x00000014$")
test.file_grep(memory_init, r"^t__DOT__hidden_mem__BRA__2__KET__ 0x0000001E$")
test.file_grep(memory_init, r"^t__DOT__hidden_mem__BRA__3__KET__ 0x00000028$")
test.file_grep(
    memory_payload,
    r"^target_path\tword_index\tvalue_hex\tword_bits\tbase_addr\taddress_unit_bytes\tendianness\tvar_name\tvar_index\twidth\tvisible$")
test.file_grep(
    memory_payload,
    r"^t\.hidden_mem\t0\t0x0000000A\t32\t0x00000000\t4\tlittle\tt__DOT__hidden_mem__BRA__0__KET__\t0\t32\t1$")
test.file_grep(
    memory_payload,
    r"^t\.hidden_mem\t3\t0x00000028\t32\t0x00000000\t4\tlittle\tt__DOT__hidden_mem__BRA__3__KET__\t3\t32\t1$")

test.passes()
