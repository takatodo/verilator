#!/usr/bin/env python3
# DESCRIPTION: Verilator: Model manifest effect classification test
#
# Code available from: https://verilator.org
#
#*************************************************************************
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import json

import vltest_bootstrap

test.scenarios('vlt')

out_filename = test.obj_dir + '/model-manifest.json'

test.compile(verilator_flags2=['--model-manifest-output', out_filename, '--stats', '--timing'],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False)

with open(out_filename, 'r', encoding='utf8') as fh:
    manifest = json.load(fh)

eval_regions = manifest['eval_regions']
if eval_regions['status'] != 'provided':
    test.error('eval region manifest is not provided')
if manifest['limitations']['eval_regions'] != 'provided':
    test.error('model limitations do not declare eval regions')
if eval_regions['classification_policy']['precedence'] != [
        'host_dependent', 'unknown', 'proven_device_clean'
]:
    test.error('incorrect eval classification precedence')

functions = {function['function_id']: function for function in eval_regions['functions']}
regions = eval_regions['regions']
if len(regions) != 1:
    test.error('main eval region is not unique')
else:
    region = regions[0]
    entry = functions.get(region['entry_function_id'])
    if entry is None or not entry['is_eval_entry']:
        test.error('main eval region does not bind the compiler-owned eval entry')
    if region['classification'] != 'host_dependent':
        test.error('DPI dependency does not propagate to main eval region')
    if region['schedule_semantics'] != 'not_provided':
        test.error('eval region overclaims schedule semantics')
    if region['convergence_semantics'] != 'not_provided':
        test.error('eval region overclaims convergence semantics')

dpi_functions = []
scheduler_functions = []
for function in functions.values():
    categories = {
        dependency['category'] for dependency in function['direct_effects']['host_dependencies']
    }
    if 'dpi_vpi' in categories:
        dpi_functions.append(function)
    if 'scheduler' in categories:
        scheduler_functions.append(function)
if not dpi_functions:
    test.error('DPI host dependency is absent from eval function inventory')
if any(function['direct_classification'] != 'host_dependent' for function in dpi_functions):
    test.error('DPI function is not directly host-dependent')
if not scheduler_functions:
    test.error('scheduler host dependency is absent from eval function inventory')
if any(function['direct_classification'] != 'host_dependent'
       for function in scheduler_functions):
    test.error('scheduler function is not directly host-dependent')

metrics = eval_regions['metrics']
classifications = [function['classification'] for function in functions.values()]
if metrics['function_count'] != len(functions):
    test.error('incorrect eval function count')
if metrics['region_count'] != len(regions):
    test.error('incorrect eval region count')
if metrics['host_dependent_function_count'] != classifications.count('host_dependent'):
    test.error('incorrect host-dependent function count')
if metrics['host_dependency_site_count'] == 0:
    test.error('eval metrics omit host dependency sites')

test.file_grep(test.stats, r'Model manifest, Eval functions emitted\s+(\d+)', len(functions))
test.file_grep(test.stats, r'Model manifest, Eval regions emitted\s+(\d+)', len(regions))

test.passes()
