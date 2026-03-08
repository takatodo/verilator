#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verifies partial hidden array preload for sim-accel
#
# When only a subset of a large array's elements (0-9 of 20) are referenced via
# constant index in the supported combinational logic, those elements appear in
# preload_targets.json's elements[] list and are applied to the state vector.
# The remaining elements (10-19) are truly hidden: they appear in the payload TSV
# as hidden words but are stored rather than applied.
#
# This test documents the current boundary between "preloadable" (logic-visible)
# and "hidden" (logic-invisible) array elements.
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

cache_dir = test.obj_dir + "/sim_accel_hidden_preload_partial_cache"
probe_dir = test.obj_dir + "/sim_accel_hidden_preload_partial_probe"
bench_dir = test.obj_dir + "/sim_accel_hidden_preload_partial_bench"
kernel_path = probe_dir + "/hidden_partial.sim_accel.kernel.cu"
vars_tsv = kernel_path + ".vars.tsv"
preload_targets_tsv = kernel_path + ".preload_targets.tsv"
preload_targets_json = kernel_path + ".preload_targets.json"
target_path = probe_dir + "/hidden_partial_mem.target.json"
bench_log = bench_dir + "/bench_run.log"
memory_payload = bench_dir + "/memory_image.payload.tsv"
target_summary = bench_dir + "/array_preload.targets.tsv"
hidden_storage = bench_dir + "/array_preload.hidden.tsv"
hidden_target_summary = bench_dir + "/array_preload.hidden_targets.tsv"
hidden_target_dir = bench_dir + "/array_preload.hidden_targets.d"
hidden_target_file = hidden_target_dir + "/0000_t.hidden_mem.tsv"

for path in [cache_dir, probe_dir, bench_dir]:
    shutil.rmtree(path, ignore_errors=True)
os.makedirs(probe_dir, exist_ok=True)

# Write a 20-word binary memory image (word_index i -> value (i+1)*10)
image_dir = tempfile.mkdtemp(prefix="sim_accel_hidden_preload_partial_", dir=test.obj_dir)
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
    + " " + test.t_dir + "/t_sim_accel_hidden_preload_partial.v")
test.run_capture(probe_cmd)

for filename in [vars_tsv, preload_targets_tsv, preload_targets_json]:
    if not os.path.exists(filename):
        test.error("Expected metadata file not found: " + filename)

# Only elements 0-9 should appear as synthetic vars
for i in range(10):
    test.file_grep(vars_tsv, rf"t__DOT__hidden_mem__BRA__{i}__KET__")
# Elements 10-19 should NOT be in vars.tsv (not referenced in logic)
for i in range(10, 20):
    test.file_grep_not(vars_tsv, rf"t__DOT__hidden_mem__BRA__{i}__KET__")

# Preload target must show depth=20
test.file_grep(preload_targets_tsv, r"memory-array-preload-v1\thidden_mem\tt\.hidden_mem\tt__DOT__hidden_mem\tt\t32\t20\t")

# Generate target descriptor
target_cmd = (
    target_gen
    + " --preload-targets-json " + preload_targets_json
    + " --description 'Generated partial hidden array target'"
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
if len(elements) != 10:
    test.error(
        "Expected 10 elements in partial preload target (only elements 0-9 referenced in logic), got "
        + str(len(elements)))
element_indices = {e["index"] for e in elements}
for i in range(10):
    if i not in element_indices:
        test.error("Expected element index " + str(i) + " to be in elements[]")
for i in range(10, 20):
    if i in element_indices:
        test.error("Element index " + str(i) + " should not be in elements[] (not referenced in logic)")

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
    + test.t_dir + "/t_sim_accel_hidden_preload_partial.v")
test.run_capture(bench_cmd)

for filename in [bench_log, memory_payload, hidden_storage, target_summary,
                 hidden_target_summary, hidden_target_file]:
    if not os.path.exists(filename):
        test.error("Expected bench artifact not found: " + filename)

# 10 words visible (elements 0-9), 10 words hidden (elements 10-19)
test.file_grep(bench_log, r"memory_image_payload_entries=20")
test.file_grep(bench_log, r"memory_image_payload_visible_entries=10")
test.file_grep(bench_log, r"memory_image_payload_hidden_entries=10")
test.file_grep(bench_log, r"array_preload_targets_loaded=1")
test.file_grep(bench_log, r"array_preload_words_loaded=20")
test.file_grep(bench_log, r"array_preload_mapped_rows_loaded=10")
test.file_grep(bench_log, r"array_preload_hidden_rows_loaded=10")
test.file_grep(bench_log, r"array_preload_hidden_only_targets=0")  # has SOME mapped rows
test.file_grep(bench_log, r"array_preload_hidden_storage_targets=1")
test.file_grep(bench_log, r"array_preload_hidden_storage_words=10")
test.file_grep(bench_log, r"array_preload_mapped_rules_applied=10")
test.file_grep(bench_log, r"direct_preload_rules_applied=10")
test.file_grep(bench_log, r"mismatch=0")

# Hidden storage should contain only elements 10-19
test.file_grep(hidden_storage, r"^t\.hidden_mem\t10\t0x0000006E\t32\t")  # value=110
test.file_grep(hidden_storage, r"^t\.hidden_mem\t19\t0x000000C8\t32\t")  # value=200
test.file_grep_not(hidden_storage, r"^t\.hidden_mem\t0\t")
test.file_grep_not(hidden_storage, r"^t\.hidden_mem\t9\t")

# Target summary: 20 total, 10 hidden, not hidden_only (has 10 mapped)
test.file_grep(
    target_summary,
    r"^t\.hidden_mem\t32\t0x00000000\t4\tlittle\t20\t10\t10\t0$")

# Payload TSV: elements 0-9 visible=1, elements 10-19 visible=0
# var_index depends on state vector order; use \d+ to avoid hardcoding it
test.file_grep(
    memory_payload,
    r"^t\.hidden_mem\t0\t0x0000000A\t32\t0x00000000\t4\tlittle\tt__DOT__hidden_mem__BRA__0__KET__\t\d+\t32\t1$")
# Hidden entries have empty var_name and -1 for var_index and width
test.file_grep(
    memory_payload,
    r"^t\.hidden_mem\t10\t0x0000006E\t32\t0x00000000\t4\tlittle\t\t-1\t-1\t0$")
test.file_grep(
    memory_payload,
    r"^t\.hidden_mem\t19\t0x000000C8\t32\t0x00000000\t4\tlittle\t\t-1\t-1\t0$")

test.passes()
