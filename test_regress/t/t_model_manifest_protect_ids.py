#!/usr/bin/env python3
# DESCRIPTION: Verilator: Model manifest protected identifier test
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
test.top_filename = 't/t_model_manifest.v'

out_filename = test.obj_dir + '/model-manifest.json'

test.compile(verilator_flags2=[
    '--model-manifest-output',
    out_filename,
    '--coverage-line',
    '--coverage-toggle',
    '--protect-ids',
    '--protect-key',
    'SECRET_KEY',
],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False)

with open(out_filename, 'r', encoding='utf8') as fh:
    manifest = json.load(fh)


def string_values(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for item in value.values():
            yield from string_values(item)
    elif isinstance(value, list):
        for item in value:
            yield from string_values(item)


serialized_values = json.dumps(list(string_values(manifest)))
for private_name in ('state_q', '__Vuser_q', 'status_if', 'status', 'done', 't_model_manifest.v'):
    if private_name in serialized_values:
        test.error('model manifest exposes protected identifier ' + private_name)

if manifest['field_count'] == 0:
    test.error('protected model manifest has no fields')
if manifest['coverage']['status'] != 'partial':
    test.error('non-toggle coverage is not reported as a partial mapping')
if not manifest['coverage']['semantic_observations']:
    test.error('protected model manifest has no semantic coverage observations')
if manifest['coverage']['metrics']['unsupported_declaration_count'] == 0:
    test.error('non-toggle coverage is not reported as unsupported')
if manifest['limitations']['coverage_mapping'] != 'partial':
    test.error('model limitations overclaim protected coverage mapping')

test.passes()
