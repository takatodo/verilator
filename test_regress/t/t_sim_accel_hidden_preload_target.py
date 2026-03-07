#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verifies hidden memory-array preload target metadata for sim-accel
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import json
import os
import shutil

import vltest_bootstrap

test.scenarios('vlt')

verilator_root = os.environ["VERILATOR_ROOT"]
target_gen = verilator_root + "/bin/verilator_sim_accel_generate_preload_target"

probe_dir = os.path.abspath(test.obj_dir + "/sim_accel_hidden_preload_probe")
kernel_path = probe_dir + "/hidden_probe.sim_accel.kernel.cu"
preload_targets_tsv = kernel_path + ".preload_targets.tsv"
preload_target_elements_tsv = kernel_path + ".preload_target_elements.tsv"
target_path = probe_dir + "/hidden_mem.target.json"

shutil.rmtree(probe_dir, ignore_errors=True)
os.makedirs(probe_dir, exist_ok=True)

probe_cmd = (
    verilator_root + "/bin/verilator"
    + " --no-std"
    + " --sim-accel-only"
    + " --sim-accel-output " + kernel_path
    + " --top-module t"
    + " " + test.t_dir + "/t_sim_accel_hidden_preload_target.v")
test.run_capture(probe_cmd)

if not os.path.exists(preload_targets_tsv):
    test.error("Expected preload_targets.tsv not found: " + preload_targets_tsv)
if not os.path.exists(preload_target_elements_tsv):
    test.error("Expected preload_target_elements.tsv not found: " + preload_target_elements_tsv)

test.file_grep(preload_targets_tsv, r"^kind\tname\ttarget_path\tast_name\thierarchy\tword_bits\tdepth\tbase_addr\taddress_unit_bytes\tendianness\tis_primary_io$")
test.file_grep(
    preload_targets_tsv,
    r"^memory-array-preload-v1\thidden_mem\tt\.hidden_mem\tt__DOT__hidden_mem\tt\t32\t4\t0\t4\tlittle\t0$")
test.file_grep(preload_target_elements_tsv, r"^target_path\tname\tindex\toffset\tbyte_count\tvar_name$")

target_cmd = (
    target_gen
    + " --preload-targets-tsv " + preload_targets_tsv
    + " --preload-target-elements-tsv " + preload_target_elements_tsv
    + " --target-path t.hidden_mem"
    + " --description 'Hidden array generated metadata'"
    + " --out " + target_path)
test.run_capture(target_cmd)

if not os.path.exists(target_path):
    test.error("Expected generated target not found: " + target_path)

with open(target_path, encoding="utf-8") as fh:
    payload = json.load(fh)
if payload.get("kind") != "memory-array-preload-v1":
    test.error("Unexpected target kind: " + str(payload.get("kind")))
if payload.get("target_path") != "t.hidden_mem":
    test.error("Unexpected target path: " + str(payload.get("target_path")))
if payload.get("word_bits") != 32:
    test.error("Unexpected word_bits: " + str(payload.get("word_bits")))
if payload.get("depth") != 4:
    test.error("Unexpected depth: " + str(payload.get("depth")))
if payload.get("elements") != []:
    test.error("Hidden preload target without constant-index reads should not expose elements")

test.passes()
