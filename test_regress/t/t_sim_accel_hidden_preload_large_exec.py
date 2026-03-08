#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verifies large hidden array (depth > materialize threshold) preload execution for sim-accel
#
# When all elements of a large array are referenced via constant index in the
# supported combinational logic, each element gets a synthetic var through the
# normal logic-resolution path. The preload target elements[] is therefore fully
# populated even though the array depth (20) exceeds kSimAccelPreloadMaterializeMaxDepth (16).
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import json
import os
import shutil
import struct
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

cache_dir = test.obj_dir + "/sim_accel_hidden_preload_large_exec_cache"
probe_dir = test.obj_dir + "/sim_accel_hidden_preload_large_exec_probe"
bench_dir = test.obj_dir + "/sim_accel_hidden_preload_large_exec_bench"
kernel_path = probe_dir + "/hidden_large_exec.sim_accel.kernel.cu"
vars_tsv = kernel_path + ".vars.tsv"
preload_targets_tsv = kernel_path + ".preload_targets.tsv"
preload_target_elements_tsv = kernel_path + ".preload_target_elements.tsv"
preload_targets_json = kernel_path + ".preload_targets.json"
target_path = probe_dir + "/hidden_large_mem.target.json"
bench_log = bench_dir + "/bench_run.log"
memory_init = bench_dir + "/memory_image.init"
memory_payload = bench_dir + "/memory_image.payload.tsv"
target_summary = bench_dir + "/array_preload.targets.tsv"
hidden_storage = bench_dir + "/array_preload.hidden.tsv"
hidden_target_summary = bench_dir + "/array_preload.hidden_targets.tsv"
hidden_target_dir = bench_dir + "/array_preload.hidden_targets.d"

for path in [cache_dir, probe_dir, bench_dir]:
    shutil.rmtree(path, ignore_errors=True)
os.makedirs(probe_dir, exist_ok=True)

# Write a 20-word binary memory image (word_index i -> value (i+1)*10)
image_dir = tempfile.mkdtemp(prefix="sim_accel_hidden_preload_large_exec_", dir=test.obj_dir)
memory_image = image_dir + "/memory.bin"
with open(memory_image, "wb") as fh:
    for i in range(20):
        fh.write(struct.pack("<I", (i + 1) * 10))

probe_cmd = (
    verilator_root + "/bin/verilator"
    + " --no-std"
    + " --sim-accel-only"
    + " --sim-accel-output " + kernel_path
    + " --top-module t"
    + " " + test.t_dir + "/t_sim_accel_hidden_preload_large_exec.v")
test.run_capture(probe_cmd)

for filename in [vars_tsv, preload_targets_tsv, preload_target_elements_tsv, preload_targets_json]:
    if not os.path.exists(filename):
        test.error("Expected metadata file not found: " + filename)

# All 20 elements must appear as synthetic vars in vars.tsv
for i in range(20):
    test.file_grep(vars_tsv, rf"t__DOT__hidden_mem__BRA__{i}__KET__")

# Preload target TSV must show depth=20
test.file_grep(preload_targets_tsv, r"memory-array-preload-v1\thidden_mem\tt\.hidden_mem\tt__DOT__hidden_mem\tt\t32\t20\t")

# Generate target descriptor from the probe output
target_cmd = (
    target_gen
    + " --preload-targets-json " + preload_targets_json
    + " --description 'Generated large hidden array target'"
    + " --target-path t.hidden_mem"
    + " --out " + target_path)
test.run_capture(target_cmd)

if not os.path.exists(target_path):
    test.error("Expected generated target not found: " + target_path)

with open(target_path, encoding="utf-8") as fh:
    target_payload = json.load(fh)
if target_payload.get("kind") != "memory-array-preload-v1":
    test.error("Unexpected target kind: " + str(target_payload.get("kind")))
if target_payload.get("depth") != 20:
    test.error("Expected depth=20, got: " + str(target_payload.get("depth")))
elements = target_payload.get("elements", [])
if len(elements) != 20:
    test.error(
        "Expected 20 elements in large array preload target (all referenced in logic), got "
        + str(len(elements)))

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
    + test.t_dir + "/t_sim_accel_hidden_preload_large_exec.v")
test.run_capture(bench_cmd)

for filename in [bench_log, memory_init, memory_payload, target_summary]:
    if not os.path.exists(filename):
        test.error("Expected bench output not found: " + filename)

# No hidden storage expected: all 20 elements are visible via synthetic vars
if os.path.exists(hidden_storage):
    test.error("Did not expect hidden storage for fully logic-referenced large array")
if os.path.exists(hidden_target_summary):
    test.error("Did not expect hidden target summary for fully logic-referenced large array")
if os.path.exists(hidden_target_dir):
    test.error("Did not expect hidden target dir for fully logic-referenced large array")

test.file_grep(bench_log, r"memory_image_target=.*hidden_large_mem\.target\.json")
test.file_grep(bench_log, r"memory_image_preload_entries=20")
test.file_grep(bench_log, r"memory_image_direct_entries=20")
test.file_grep(bench_log, r"memory_image_payload_entries=20")
test.file_grep(bench_log, r"memory_image_payload_visible_entries=20")
test.file_grep(bench_log, r"memory_image_payload_hidden_entries=0")
test.file_grep(bench_log, r"array_preload_targets_loaded=1")
test.file_grep(bench_log, r"array_preload_words_loaded=20")
test.file_grep(bench_log, r"array_preload_mapped_rows_loaded=20")
test.file_grep(bench_log, r"array_preload_hidden_rows_loaded=0")
test.file_grep(bench_log, r"array_preload_hidden_only_targets=0")
test.file_grep(bench_log, r"array_preload_hidden_storage_targets=0")
test.file_grep(bench_log, r"array_preload_hidden_storage_words=0")
test.file_grep(bench_log, r"array_preload_mapped_rules_applied=20")
test.file_grep(bench_log, r"array_preload_mapped_values_applied=")  # 20 * nstates
test.file_grep(bench_log, r"direct_preload_rules_applied=20")
test.file_grep(bench_log, r"mismatch=0")

# Verify memory_image.init has correct values: word i -> (i+1)*10 in hex
test.file_grep(memory_init, r"^t__DOT__hidden_mem__BRA__0__KET__ 0x0000000A$")   # 10
test.file_grep(memory_init, r"^t__DOT__hidden_mem__BRA__9__KET__ 0x00000064$")   # 100
test.file_grep(memory_init, r"^t__DOT__hidden_mem__BRA__10__KET__ 0x0000006E$")  # 110
test.file_grep(memory_init, r"^t__DOT__hidden_mem__BRA__19__KET__ 0x000000C8$")  # 200

# Verify payload TSV covers all 20 words as visible
test.file_grep(
    memory_payload,
    r"^target_path\tword_index\tvalue_hex\tword_bits\tbase_addr\taddress_unit_bytes\tendianness\tvar_name\tvar_index\twidth\tvisible$")
# var_index depends on state vector order; use \d+ to avoid hardcoding it
test.file_grep(
    memory_payload,
    r"^t\.hidden_mem\t0\t0x0000000A\t32\t0x00000000\t4\tlittle\tt__DOT__hidden_mem__BRA__0__KET__\t\d+\t32\t1$")
test.file_grep(
    memory_payload,
    r"^t\.hidden_mem\t19\t0x000000C8\t32\t0x00000000\t4\tlittle\tt__DOT__hidden_mem__BRA__19__KET__\t\d+\t32\t1$")

# Target summary: all 20 words visible, 0 hidden, not hidden_only
test.file_grep(
    target_summary,
    r"^t\.hidden_mem\t32\t0x00000000\t4\tlittle\t20\t0\t20\t0$")

test.passes()
