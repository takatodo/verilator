#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verifies hidden-only memory-array preload payloads remain unapplied in sim-accel bench runtime
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

cache_dir = test.obj_dir + "/sim_accel_hidden_preload_unapplied_cache"
probe_dir = test.obj_dir + "/sim_accel_hidden_preload_unapplied_probe"
bench_dir = test.obj_dir + "/sim_accel_hidden_preload_unapplied_bench"
kernel_path = probe_dir + "/hidden_unapplied.sim_accel.kernel.cu"
preload_targets_tsv = kernel_path + ".preload_targets.tsv"
preload_target_elements_tsv = kernel_path + ".preload_target_elements.tsv"
preload_targets_json = kernel_path + ".preload_targets.json"
target_path = probe_dir + "/hidden_mem.target.json"
bench_log = bench_dir + "/bench_run.log"
memory_payload = bench_dir + "/memory_image.payload.tsv"
hidden_storage = bench_dir + "/array_preload.hidden.tsv"
target_summary = bench_dir + "/array_preload.targets.tsv"
hidden_target_summary = bench_dir + "/array_preload.hidden_targets.tsv"
hidden_target_dir = bench_dir + "/array_preload.hidden_targets.d"
hidden_target_file = hidden_target_dir + "/0000_t.hidden_mem.tsv"

for path in [cache_dir, probe_dir, bench_dir]:
    shutil.rmtree(path, ignore_errors=True)
os.makedirs(probe_dir, exist_ok=True)

image_dir = tempfile.mkdtemp(prefix="sim_accel_hidden_preload_unapplied_", dir=test.obj_dir)
memory_image = image_dir + "/memory.bin"
with open(memory_image, "wb") as fh:
    for word in range(17):
        value = (word + 1) * 10
        fh.write(value.to_bytes(4, byteorder="little", signed=False))

probe_cmd = (
    verilator_root + "/bin/verilator"
    + " --no-std"
    + " --sim-accel-only"
    + " --sim-accel-output " + kernel_path
    + " --top-module t"
    + " " + test.t_dir + "/t_sim_accel_hidden_preload_unmapped.v")
test.run_capture(probe_cmd)

for filename in [preload_targets_tsv, preload_target_elements_tsv, preload_targets_json]:
    if not os.path.exists(filename):
        test.error("Expected generated preload metadata not found: " + filename)

target_cmd = (
    target_gen
    + " --preload-targets-json " + preload_targets_json
    + " --description 'Generated hidden-only preload target'"
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
if target_payload.get("elements") != []:
    test.error("Expected hidden-only target to have no visible elements")

bench_cmd = (
    verilator_root + "/bin/verilator --sim-accel-bench"
    + " --top-module t"
    + " --outdir " + bench_dir
    + " --nstates 2048"
    + " --gpu-reps 4"
    + " --cpu-reps 2"
    + " --init-mode zero"
    + " --compile-cache-dir " + cache_dir
    + " --memory-image " + memory_image
    + " --memory-image-target " + target_path
    + " --memory-image-format bin"
    + " -- "
    + test.t_dir + "/t_sim_accel_hidden_preload_unmapped.v")
test.run_capture(bench_cmd)

for filename in [bench_log, memory_payload, hidden_storage, target_summary, hidden_target_summary, hidden_target_file]:
    if not os.path.exists(filename):
        test.error("Expected bench artifact not found: " + filename)

test.file_grep(bench_log, r"memory_image_target=.*hidden_mem\.target\.json")
test.file_grep(bench_log, r"memory_image_preload_entries=0")
test.file_grep(bench_log, r"memory_image_direct_entries=0")
test.file_grep(bench_log, r"memory_image_payload_tsv=.*memory_image\.payload\.tsv")
test.file_grep(bench_log, r"memory_image_payload_entries=17")
test.file_grep(bench_log, r"memory_image_payload_visible_entries=0")
test.file_grep(bench_log, r"memory_image_payload_hidden_entries=17")
test.file_grep(bench_log, r"direct_preload_file_count=1")
test.file_grep(bench_log, r"direct_preload_rules_applied=0")
test.file_grep(bench_log, r"direct_preload_values_applied=0")
test.file_grep(bench_log, r"array_preload_payload_file_count=1")
test.file_grep(bench_log, r"array_preload_payload_files_loaded=1")
test.file_grep(bench_log, r"array_preload_targets_loaded=1")
test.file_grep(bench_log, r"array_preload_words_loaded=17")
test.file_grep(bench_log, r"array_preload_mapped_rows_loaded=0")
test.file_grep(bench_log, r"array_preload_hidden_rows_loaded=17")
test.file_grep(bench_log, r"array_preload_hidden_only_targets=1")
test.file_grep(bench_log, r"array_preload_hidden_storage_targets=1")
test.file_grep(bench_log, r"array_preload_hidden_storage_words=17")
test.file_grep(bench_log, r"array_preload_target_summary_tsv=array_preload\.targets\.tsv")
test.file_grep(bench_log, r"array_preload_hidden_storage_tsv=array_preload\.hidden\.tsv")
test.file_grep(bench_log, r"array_preload_hidden_target_summary_tsv=array_preload\.hidden_targets\.tsv")
test.file_grep(bench_log, r"array_preload_hidden_target_dir=array_preload\.hidden_targets\.d")
test.file_grep(bench_log, r"array_preload_mapped_rules_applied=0")
test.file_grep(bench_log, r"array_preload_mapped_values_applied=0")
test.file_grep(bench_log, r"array_preload_lines_ignored=0")
test.file_grep(bench_log, r"mismatch=0")
test.file_grep(
    hidden_storage,
    r"^target_path\tword_index\tvalue_hex\tword_bits\tbase_addr\taddress_unit_bytes\tendianness$")
test.file_grep(hidden_storage, r"^t\.hidden_mem\t0\t0x0000000A\t32\t0x00000000\t4\tlittle$")
test.file_grep(hidden_storage, r"^t\.hidden_mem\t16\t0x000000AA\t32\t0x00000000\t4\tlittle$")
test.file_grep(
    target_summary,
    r"^target_path\tword_bits\tbase_addr\taddress_unit_bytes\tendianness\ttotal_words\thidden_words\tmapped_values\thidden_only$")
test.file_grep(
    target_summary,
    r"^t\.hidden_mem\t32\t0x00000000\t4\tlittle\t17\t17\t0\t1$")
test.file_grep(
    hidden_target_summary,
    r"^target_path\tstorage_tsv\thidden_words$")
test.file_grep(
    hidden_target_summary,
    r"^t\.hidden_mem\tarray_preload\.hidden_targets\.d/0000_t\.hidden_mem\.tsv\t17$")
test.file_grep(
    hidden_target_file,
    r"^target_path\tword_index\tvalue_hex\tword_bits\tbase_addr\taddress_unit_bytes\tendianness$")
test.file_grep(hidden_target_file, r"^t\.hidden_mem\t0\t0x0000000A\t32\t0x00000000\t4\tlittle$")
test.file_grep(hidden_target_file, r"^t\.hidden_mem\t16\t0x000000AA\t32\t0x00000000\t4\tlittle$")
test.file_grep(memory_payload, r"^target_path\tword_index\tvalue_hex\tword_bits\tbase_addr\taddress_unit_bytes\tendianness\tvar_name\tvar_index\twidth\tvisible$")
test.file_grep(memory_payload, r"^t\.hidden_mem\t0\t0x0000000A\t32\t0x00000000\t4\tlittle\t\t-1\t-1\t0$")
test.file_grep(memory_payload, r"^t\.hidden_mem\t16\t0x000000AA\t32\t0x00000000\t4\tlittle\t\t-1\t-1\t0$")

test.passes()
