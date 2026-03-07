#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import os
import json
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

cache_dir = test.obj_dir + "/sim_accel_scenario_batch_cache"
batch_dir = test.obj_dir + "/sim_accel_scenario_batch"
summary_json = batch_dir + "/scenario_batch.json"
summary_tsv = batch_dir + "/scenario_batch.tsv"
seed1_log = batch_dir + "/scenarios/iter000008/driver_stdout.log"
seed2_log = batch_dir + "/scenarios/iter000010/driver_stdout.log"
program_hex1 = batch_dir + "/scenario_inputs/iter000008/program.hex"
program_hex2 = batch_dir + "/scenario_inputs/iter000010/program.hex"

for path in [cache_dir, batch_dir]:
    shutil.rmtree(path, ignore_errors=True)

program_hex_dir = tempfile.mkdtemp(prefix="sim_accel_program_hex_", dir=test.obj_dir)
program_hex_path = program_hex_dir + "/program.hex"
manifest_path = program_hex_dir + "/scenarios.json"
with open(program_hex_path, "w", encoding="utf-8") as fh:
    fh.write("@10000000\n")
    fh.write("01 00 00 00\n")
with open(manifest_path, "w", encoding="utf-8") as fh:
    json.dump(
        [
            {"name": "iter000008", "program_hex": program_hex_path, "iterations": 8,
             "init_mode": "counter", "init_seed": 1},
            {"name": "iter000010", "program_hex": program_hex_path, "iterations": 10,
             "init_mode": "counter", "init_seed": 2},
        ],
        fh,
    )

cmd = (
    os.environ["VERILATOR_ROOT"] + "/bin/verilator_sim_accel_scenario_batch"
    + " --verilator " + os.environ["VERILATOR_ROOT"] + "/bin/verilator"
    + " --top-module t"
    + " --outdir " + batch_dir
    + " --nstates 2048"
    + " --gpu-reps 4"
    + " --cpu-reps 2"
    + " --assigns-per-kernel 2"
    + " --compile-cache-dir " + cache_dir
    + " --scenario-manifest " + manifest_path
    + " -- "
    + test.t_dir + "/t_sim_accel_bench_exec.v")

test.run_capture(cmd)

for filename in [summary_json, summary_tsv, seed1_log, seed2_log, program_hex1, program_hex2]:
    if not os.path.exists(filename):
        test.error("Expected output file not found: " + filename)

test.file_grep(summary_json, r'"schema_version": "sim-accel-scenario-batch-v1"')
test.file_grep(summary_json, r'"scenario_count": 2')
test.file_grep(summary_json, r'"success_count": 2')
test.file_grep(summary_json, r'"program_hex_materialized": ".*scenario_inputs/iter000008/program.hex"')
test.file_grep(summary_json, r'"nvcc_cache_mode_counts": \{')
test.file_grep(summary_json, r'"hit": 1')
test.file_grep(summary_json, r'"miss": 1')
test.file_grep(summary_json, r'"verilator_artifact_cache_mode_counts": \{')
test.file_grep(summary_tsv, r"iter000008")
test.file_grep(summary_tsv, r"iter000010")
test.file_grep(seed1_log, r"nvcc_cache_mode=miss")
test.file_grep(seed2_log, r"nvcc_cache_mode=hit")
test.file_grep(seed1_log, r"verilator_artifact_cache_mode=miss")
test.file_grep(seed2_log, r"verilator_artifact_cache_mode=hit")
test.file_grep(program_hex1, r"08 00 00 00")
test.file_grep(program_hex2, r"0A 00 00 00")

test.passes()
