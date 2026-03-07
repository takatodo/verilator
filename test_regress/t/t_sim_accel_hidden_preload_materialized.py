#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verifies small hidden preload arrays are materialized into direct preload vars
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

cache_dir = test.obj_dir + "/sim_accel_hidden_preload_materialized_cache"
probe_dir = test.obj_dir + "/sim_accel_hidden_preload_materialized_probe"
bench_dir = test.obj_dir + "/sim_accel_hidden_preload_materialized_bench"
kernel_path = probe_dir + "/hidden_materialized.sim_accel.kernel.cu"
preload_targets_json = kernel_path + ".preload_targets.json"
target_path = probe_dir + "/hidden_mem.target.json"
bench_log = bench_dir + "/bench_run.log"
memory_payload = bench_dir + "/memory_image.payload.tsv"

for path in [cache_dir, probe_dir, bench_dir]:
    shutil.rmtree(path, ignore_errors=True)
os.makedirs(probe_dir, exist_ok=True)

image_dir = tempfile.mkdtemp(prefix="sim_accel_hidden_preload_materialized_", dir=test.obj_dir)
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
    + " " + test.t_dir + "/t_sim_accel_hidden_preload_target.v")
test.run_capture(probe_cmd)

if not os.path.exists(preload_targets_json):
    test.error("Expected preload_targets.json not found: " + preload_targets_json)

target_cmd = (
    target_gen
    + " --preload-targets-json " + preload_targets_json
    + " --description 'Generated hidden preload target with materialized elements'"
    + " --target-path t.hidden_mem"
    + " --out " + target_path)
test.run_capture(target_cmd)

with open(target_path, encoding="utf-8") as fh:
    target_payload = json.load(fh)
elements = target_payload.get("elements", [])
if len(elements) != 4:
    test.error("Expected 4 materialized hidden memory elements, got " + str(len(elements)))

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
    + test.t_dir + "/t_sim_accel_hidden_preload_target.v")
test.run_capture(bench_cmd)

test.file_grep(bench_log, r"memory_image_payload_entries=4")
test.file_grep(bench_log, r"memory_image_payload_visible_entries=4")
test.file_grep(bench_log, r"memory_image_payload_hidden_entries=0")
test.file_grep(bench_log, r"array_preload_mapped_rows_loaded=4")
test.file_grep(bench_log, r"array_preload_hidden_rows_loaded=0")
test.file_grep(bench_log, r"array_preload_hidden_only_targets=0")
test.file_grep(bench_log, r"array_preload_mapped_rules_applied=4")
test.file_grep(bench_log, r"array_preload_mapped_values_applied=8192")
test.file_grep(memory_payload, r"^t\.hidden_mem\t0\t0x0000000A\t32\t0x00000000\t4\tlittle\tt__DOT__hidden_mem__BRA__0__KET__")
test.file_grep(memory_payload, r"^t\.hidden_mem\t3\t0x00000028\t32\t0x00000000\t4\tlittle\tt__DOT__hidden_mem__BRA__3__KET__")

test.passes()
