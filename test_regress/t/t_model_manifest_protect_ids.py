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
    '--protect-ids',
    '--protect-key',
    'SECRET_KEY',
],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False)

with open(out_filename, 'r', encoding='utf8') as fh:
    manifest = json.load(fh)

serialized = json.dumps(manifest)
for private_name in ('state_q', '__Vuser_q', 'status_if', 'status', 'done', 't_model_manifest.v'):
    if private_name in serialized:
        test.error('model manifest exposes protected identifier ' + private_name)

if manifest['field_count'] == 0:
    test.error('protected model manifest has no fields')

test.passes()
