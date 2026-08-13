#!/usr/bin/env python3
# DESCRIPTION: Verilator: Model manifest deterministic output test
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

import vltest_bootstrap

test.scenarios('vlt')

manifests = []
for suffix in ('first', 'second'):
    obj_dir = test.obj_dir + '/' + suffix
    manifest = obj_dir + '/model-manifest.json'
    test.mkdir_ok(obj_dir)
    test.run(logfile=obj_dir + '/vlt_compile.log',
             cmd=[
                 'perl', os.environ['VERILATOR_ROOT'] + '/bin/verilator', '--cc', '--Mdir',
                 obj_dir, '--prefix', 'Vt', '--top-module', 't', '--no-timing',
                 '--model-manifest-output', manifest, 't/t_model_manifest.v'
             ])
    manifests.append(manifest)

test.files_identical(manifests[0], manifests[1])

test.passes()
