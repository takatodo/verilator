#!/usr/bin/env python3
# DESCRIPTION: Verilator: Model manifest toggle coverage mapping test
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

import hashlib
import json
import re
from collections import defaultdict
from pathlib import Path

import vltest_bootstrap

test.scenarios('vlt')
test.top_filename = 't/t_cover_toggle.v'

out_filename = test.obj_dir + '/model-manifest.json'

test.compile(verilator_flags2=[
    '--cc',
    '--coverage-toggle',
    '--model-manifest-output',
    out_filename,
    '--stats',
],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False)

with open(out_filename, 'r', encoding='utf8') as fh:
    manifest = json.load(fh)

coverage = manifest['coverage']
metrics = coverage['metrics']

if coverage['status'] != 'provided':
    test.error('toggle coverage mapping is not complete')
if coverage['authority'] != 'verilator_coverage_lowering':
    test.error('toggle coverage mapping has the wrong authority')
if coverage['semantic_id_scheme'] != 'sha256_length_prefixed_utf8_v1':
    test.error('toggle coverage mapping has an unsupported identity scheme')
if coverage['counter_semantics'] != {
        'word_bits': 32,
        'cpp_type': 'uint32_t',
        'hit': 'nonzero_word',
        'alias_aggregation': 'logical_or',
        'transition_order': ['1->0', '0->1']
}:
    test.error('toggle coverage counter semantics are incorrect')

expected_metrics = {
    'toggle_template_count': 63,
    'lowering_declaration_count': 63,
    'semantic_observation_count': 252,
    'semantic_binding_count': 252,
    'storage_count': 2,
    'physical_word_count': 196,
    'aliased_physical_word_count': 24,
    'maximum_semantic_observations_per_physical_word': 6,
    'update_template_count': 96,
    'update_site_count': 96,
    'update_region_count': 49,
    'unsupported_declaration_count': 0,
    'uninstantiated_local_declaration_count': 0,
    'unupdated_physical_word_count': 0,
    'update_only_physical_word_count': 0,
}
if metrics != expected_metrics:
    test.error('toggle coverage mapping metrics are incorrect')
if manifest['limitations']['coverage_mapping'] != 'provided':
    test.error('model limitations disagree with the coverage mapping')


def framed_id(kind, fields):
    framed = kind.encode()
    for name, value in fields:
        name_bytes = name.encode()
        value_bytes = value.encode()
        framed += str(len(name_bytes)).encode() + b':' + name_bytes
        framed += str(len(value_bytes)).encode() + b':' + value_bytes
    return kind + ':' + hashlib.sha256(framed).hexdigest()


storages = coverage['storages']
storage_by_id = {storage['storage_id']: storage for storage in storages}
if len(storage_by_id) != len(storages):
    test.error('coverage storage identities are not unique')
for storage in storages:
    binding = storage['generated_binding']
    expected_id = framed_id('coverage-storage:v1', [
        ('semantic_instance_id', storage['semantic_instance_id']),
        ('container', binding['container']),
        ('member', binding['member']),
        ('storage', binding['storage']),
    ])
    if storage['storage_id'] != expected_id:
        test.error('coverage storage identity is incorrect')
    header = Path(test.obj_dir) / (binding['container'] + '.h')
    text = header.read_text(encoding='utf8')
    pattern = (r'\b' + re.escape(binding['member']) + r'\s*\[\s*' + str(storage['word_count']) +
               r'\s*\]')
    if re.search(pattern, text) is None:
        test.error('coverage storage binding is absent from generated header')

declarations = coverage['lowering_declarations']
declaration_by_id = {declaration['lowering_id']: declaration for declaration in declarations}
if len(declaration_by_id) != len(declarations):
    test.error('coverage lowering identities are not unique')
for declaration in declarations:
    source = declaration['source']
    range_ = declaration['range']
    expected_id = framed_id('toggle-lowering:v1', [
        ('semantic_instance_id', declaration['semantic_instance_id']),
        ('filename', source['file']),
        ('line', str(source['line'])),
        ('column', str(source['column'])),
        ('hierarchy_suffix', declaration['hierarchy_suffix']),
        ('page', declaration['page']),
        ('comment', declaration['comment']),
        ('begin', str(range_['begin'])),
        ('end', str(range_['end'])),
        ('ranged', str(range_['ranged']).lower()),
        ('template_ordinal', str(declaration['template_ordinal'])),
    ])
    if declaration['lowering_id'] != expected_id:
        test.error('coverage lowering identity is incorrect')
    storage = storage_by_id.get(declaration['storage_id'])
    width = abs(range_['end'] - range_['begin']) + 1
    if storage is None or declaration['raw_base_word'] + 2 * width > storage['word_count']:
        test.error('coverage lowering lies outside generated storage')

observations = coverage['semantic_observations']
observation_by_id = {observation['semantic_id']: observation for observation in observations}
if len(observation_by_id) != len(observations):
    test.error('semantic coverage identities are not unique')
for observation in observations:
    source = observation['source']
    bit_index = observation.get('bit_index', 'not_applicable')
    expected_id = framed_id('toggle-observation:v1', [
        ('semantic_instance_id', observation['semantic_instance_id']),
        ('filename', source['file']),
        ('line', str(source['line'])),
        ('column', str(source['column'])),
        ('hierarchy_suffix', observation['hierarchy_suffix']),
        ('page', observation['page']),
        ('comment', observation['comment']),
        ('bit_index', str(bit_index)),
        ('transition', observation['transition']),
    ])
    if observation['semantic_id'] != expected_id:
        test.error('semantic coverage identity is incorrect')

members_by_word = defaultdict(set)
for binding in coverage['bindings']:
    if binding['semantic_id'] not in observation_by_id:
        test.error('coverage binding refers to an unknown semantic identity')
    if binding['lowering_id'] not in declaration_by_id:
        test.error('coverage binding refers to an unknown lowering identity')
    members_by_word[binding['physical_word_id']].add(binding['semantic_id'])

physical_words = coverage['physical_words']
physical_by_id = {word['physical_word_id']: word for word in physical_words}
if len(physical_by_id) != len(physical_words):
    test.error('physical coverage word identities are not unique')
for word in physical_words:
    expected_physical_id = framed_id('coverage-word:v1', [
        ('storage_id', word['storage_id']),
        ('raw_word_index', str(word['raw_word_index'])),
    ])
    members = sorted(members_by_word[word['physical_word_id']])
    expected_alias_id = framed_id('coverage-alias-group:v1',
                                  [('member', member) for member in members])
    if (word['physical_word_id'] != expected_physical_id or word['member_semantic_ids'] != members
            or word['member_count'] != len(members)
            or word['alias_group_id'] != expected_alias_id):
        test.error('physical coverage alias mapping is incorrect')

updated_words = set()
for region in coverage['update_regions']:
    for offset in range(2 * region['width_bits']):
        updated_words.add(
            framed_id('coverage-word:v1', [
                ('storage_id', region['storage_id']),
                ('raw_word_index', str(region['raw_base_word'] + offset)),
            ]))
if updated_words != set(physical_by_id):
    test.error('coverage update regions do not match semantic physical words')

if declarations != sorted(declarations, key=lambda row: row['lowering_id']):
    test.error('coverage lowerings are not deterministically ordered')
if observations != sorted(observations, key=lambda row: row['semantic_id']):
    test.error('coverage observations are not deterministically ordered')
if physical_words != sorted(physical_words,
                            key=lambda row: (row['storage_id'], row['raw_word_index'])):
    test.error('physical coverage words are not deterministically ordered')

test.file_grep(test.stats, r'Model manifest, Coverage observations emitted\s+(\d+)', 252)
test.file_grep(test.stats, r'Model manifest, Coverage physical words emitted\s+(\d+)', 196)

test.passes()
