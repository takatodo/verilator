#!/usr/bin/env python3
# DESCRIPTION: Verilator: Model manifest checkpoint field selection test
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

test.scenarios('vltmt')
test.top_filename = 't/t_model_manifest.v'

out_filename = test.obj_dir + '/model-manifest.json'

test.compile(verilator_flags2=['--cc', '--model-manifest-output', out_filename],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False,
             threads=2)

with open(out_filename, 'r', encoding='utf8') as fh:
    manifest = json.load(fh)

excluded = [
    field for field in manifest['fields']
    if field['checkpoint_membership']['status'] == 'excluded'
]
if not any(field['checkpoint_membership']['reason'] == 'mtask_state'
           for field in excluded):
    test.error('checkpoint membership does not exclude transient MTask state')
if manifest['checkpoint_projection']['excluded_definition_field_count'] != len(excluded):
    test.error('incorrect excluded checkpoint field count')
if manifest['limitations']['pointer_free_checkpoint'] != 'not_provided':
    test.error('model manifest overclaims a pointer-free checkpoint')

test.passes()
