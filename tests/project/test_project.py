# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Regression tests for acceptance and public/private boundaries."""

import copy
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('project_checker', ROOT / 'tools/check-project.py')
CHECKER = importlib.util.module_from_spec(SPEC)
exec(compile(Path(SPEC.origin).read_bytes(), SPEC.origin, 'exec'), CHECKER.__dict__)


class ContractTests(unittest.TestCase):
    def setUp(self):
        self.catalog = CHECKER.parse((ROOT / 'spec/dod.json').read_bytes())
        self.baseline = CHECKER.parse((ROOT / 'tests/project/acceptance_ids.json').read_bytes())

    def test_existing_ids_mandatory_cases_and_scopes_preserved(self):
        CHECKER.validate_catalog(self.catalog)
        CHECKER.validate_preserved_ids(self.catalog, self.baseline)

    def test_new_requirement_is_allowed_without_a_count_or_hash_override(self):
        node = copy.deepcopy(self.catalog['domains'][0]['requirements'][0])
        node['id'] = '2d-native.additional-review'
        node['criteria_ref'] = 'tests/acceptance/2d-native.md#' + node['id']
        node['cases'][0]['id'] = '2d-native.additional-case'
        node['cases'][0]['criteria_ref'] = 'tests/acceptance/2d-native.md#' + node['cases'][0]['id']
        self.catalog['domains'][0]['requirements'].append(node)
        result = CHECKER.validate_catalog(self.catalog)
        CHECKER.validate_preserved_ids(self.catalog, self.baseline)
        retired = sum(1 for e in self.catalog['retired'] if e['kind'] == 'requirement')
        self.assertEqual(result['requirements'], len(self.baseline['requirements']) - retired + 1)

    def test_lost_requirement_and_optionalizing_are_refused(self):
        for change in ('remove', 'optional'):
            self.setUp()
            requirements = self.catalog['domains'][0]['requirements']
            if change == 'remove':
                requirements.pop(0)
            else:
                requirements[0]['mandatory'] = False
            with self.subTest(change=change), self.assertRaises(ValueError):
                CHECKER.validate_preserved_ids(self.catalog, self.baseline)

    def test_case_cannot_be_removed_reparented_or_promoted(self):
        for change in ('remove', 'reparent', 'scope'):
            self.setUp()
            first, second = self.catalog['domains'][0]['requirements'][:2]
            if change == 'remove':
                second['cases'].pop()
            elif change == 'reparent':
                first['cases'].append(second['cases'].pop())
            else:
                second['cases'][0]['required_backend'] = 'CPU_VM'
            with self.subTest(change=change), self.assertRaises(ValueError):
                CHECKER.validate_preserved_ids(self.catalog, self.baseline)

    def test_dependency_cycles_and_unknown_nodes_refused(self):
        for location in ('domains', 'integration_milestones'):
            self.setUp()
            self.catalog[location][0]['prerequisites'] = [self.catalog[location][0]['id']]
            with self.subTest(location=location), self.assertRaisesRegex(ValueError, 'cycle'):
                CHECKER.validate_catalog(self.catalog)
        self.setUp()
        self.catalog['domains'][0]['prerequisites'] = ['unknown']
        with self.assertRaisesRegex(ValueError, 'unknown prerequisite'):
            CHECKER.validate_catalog(self.catalog)

    def test_criteria_anchor_must_belong_to_its_own_id(self):
        first, second = self.catalog['domains'][0]['requirements'][:2]
        first['criteria_ref'] = second['criteria_ref']
        with self.assertRaisesRegex(ValueError, 'stable ID'):
            CHECKER.validate_catalog(self.catalog)

    def test_criteria_paths_are_canonical_local_markdown(self):
        node = self.catalog['domains'][0]['requirements'][0]
        for path in (
            '../escape.md',
            '/absolute.md',
            './alias.md',
            'dir\\file.md',
            'https://host/a.md',
        ):
            node['criteria_ref'] = path + '#' + node['id']
            with self.subTest(path=path), self.assertRaises(ValueError):
                CHECKER.validate_catalog(self.catalog)

    def test_shared_parameter_references_cannot_be_missing_or_cyclic(self):
        node = self.catalog['integration_milestones'][0]['cases'][0]
        for target in ('missing.case', node['id']):
            node['parameters']['fault_matrix_case'] = target
            with self.subTest(target=target), self.assertRaises(ValueError):
                CHECKER.validate_catalog(self.catalog)

    def test_removed_evidence_disposition_policy_is_not_a_valid_parameter_reference(self):
        case = self.catalog['domains'][0]['requirements'][0]['cases'][0]
        case['parameters']['artifact_stage_policy'] = (
            'Apply coverage_policy.evidence_dispositions to this outcome.'
        )
        with self.assertRaisesRegex(ValueError, 'removed evidence disposition policy'):
            CHECKER.validate_catalog(self.catalog)

    def test_expanded_raster_lifetime_and_api_boundaries_are_present(self):
        cases = {
            c['id']: c
            for d in self.catalog['domains']
            for r in d['requirements']
            for c in r['cases']
        }
        self.assertTrue(cases['gles2.winsys-contexts']['parameters']['concurrency'])
        self.assertTrue(cases['gles2.winsys-contexts']['parameters']['client_failure'])
        self.assertTrue(cases['gles2.raster-states']['parameters']['coordinate_cross_cases'])
        self.assertTrue(cases['gles2.fragment-state-matrix']['parameters']['depth_cross_cases'])
        self.assertTrue(cases['gles2.fragment-state-matrix']['parameters']['source_sample_state'])
        self.assertTrue(cases['gles2.fbo-states']['parameters']['size_and_pressure'])
        self.assertTrue(cases['gles2.spec-to-case-map']['parameters']['limit_boundaries'])

    def test_mixed_scope_requires_every_constituent_backend(self):
        node = next(
            r
            for d in self.catalog['domains']
            for r in d['requirements']
            if r['id'] == 'release.hang-recovery'
        )
        self.assertEqual(node['required_backend'], 'MIXED')
        node['cases'][1]['required_backend'] = 'CPU_VM'
        node['cases'][1]['runner_suite']['backend'] = 'CPU_VM'
        with self.assertRaisesRegex(ValueError, 'aggregate backend'):
            CHECKER.validate_catalog(self.catalog)

    def test_run_status_is_not_an_acceptance_parameter(self):
        node = self.catalog['domains'][0]['requirements'][0]
        node['status'] = 'HW_PASS'
        with self.assertRaisesRegex(ValueError, 'requirement fields'):
            CHECKER.validate_catalog(self.catalog)

    def test_runner_registration_is_allowed_but_must_match_case_scope(self):
        case = self.catalog['integration_milestones'][0]['cases'][0]
        case['runner_suite'] = dict(
            entrypoint='tools/check-gem-policy.py',
            arguments=[],
            backend='CPU_VM',
            repository='driver',
            builder='C3',
        )
        CHECKER.validate_catalog(self.catalog)
        case['runner_suite']['backend'] = 'HARDWARE'
        with self.assertRaisesRegex(ValueError, 'scope mismatch'):
            CHECKER.validate_catalog(self.catalog)
        for field, value in (('repository', 'elsewhere'), ('builder', ''), ('entrypoint', '../x')):
            self.setUp()
            case = self.catalog['integration_milestones'][0]['cases'][0]
            case['runner_suite'] = dict(
                entrypoint='tools/check-gem-policy.py',
                arguments=[],
                backend='CPU_VM',
                repository='driver',
                builder='C3',
            )
            case['runner_suite'][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                CHECKER.validate_catalog(self.catalog)

    def test_every_domain_is_in_m1_or_after_it(self):
        policy = self.catalog['coverage_policy']
        policy['after_m1_domains'].remove('mpeg2')
        with self.assertRaisesRegex(ValueError, 'in M1 or after it'):
            CHECKER.validate_catalog(self.catalog)
        self.setUp()
        self.catalog['coverage_policy']['m1_domains'].append('mpeg2')
        with self.assertRaisesRegex(ValueError, 'in M1 or after it'):
            CHECKER.validate_catalog(self.catalog)

    def test_an_id_is_retired_never_deleted(self):
        retired = self.catalog['retired']
        entry = next(e for e in retired if e['id'] == 'integration.typed-kms')
        retired.remove(entry)
        with self.assertRaisesRegex(ValueError, 'lost or reparented'):
            CHECKER.validate_preserved_ids(self.catalog, self.baseline)
        self.setUp()
        self.catalog['retired'].append(
            dict(id='gles2.alu-vectors', kind='case', parent='gles2.arithmetic', reason='x',
                 replaced_by=[])
        )
        with self.assertRaisesRegex(ValueError, 'retired ID is live'):
            CHECKER.validate_catalog(self.catalog)

    def test_case_list_path_is_bound_to_the_case_id(self):
        case = self.catalog['domains'][0]['requirements'][1]['cases'][0]
        self.assertEqual(case['case_list'], 'spec/cases/' + case['id'] + '.json')
        case['case_list'] = 'spec/cases/other.json'
        with self.assertRaisesRegex(ValueError, 'case list path'):
            CHECKER.validate_catalog(self.catalog)

    def test_malformed_cases_and_unsafe_artifacts_refused(self):
        for field, value in [
            ('parameters', {}),
            ('artifacts', []),
            ('artifacts', ['../escape']),
            ('artifacts', ['same', 'same']),
            ('required_backend', []),
            ('runner_suite', 'execute.py'),
        ]:
            self.setUp()
            self.catalog['domains'][0]['requirements'][0]['cases'][0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                CHECKER.validate_catalog(self.catalog)

    def test_release_and_integration_scopes_are_distinct(self):
        self.catalog['required_for_release'].remove('mpeg2')
        with self.assertRaisesRegex(ValueError, 'release domain coverage'):
            CHECKER.validate_catalog(self.catalog)
        self.setUp()
        self.catalog['integration_milestones'][0]['closes_domains'] = True
        with self.assertRaisesRegex(ValueError, 'cannot close'):
            CHECKER.validate_catalog(self.catalog)

    def test_gears_second_application_and_decoder_prerequisites_preserved(self):
        milestone = next(
            m for m in self.catalog['integration_milestones'] if m['id'] == 'integration.gears'
        )
        gears, heads = milestone['cases']
        self.assertEqual(gears['id'], 'integration.gears-acceptance')
        self.assertEqual(
            [m['size'] for m in gears['parameters']['modes']], [[640, 640], [1280, 800]]
        )
        self.assertTrue(all(m['minimum_seconds'] >= 120 for m in gears['parameters']['modes']))
        self.assertEqual(gears['parameters']['swap_control']['requested_interval'], 0)
        self.assertIn('swap-control.original.json', gears['artifacts'])
        self.assertGreaterEqual(heads['parameters']['minimum_local_connections'], 2)
        self.assertEqual(
            next(d for d in self.catalog['domains'] if d['id'] == 'mpeg2')['prerequisites'], []
        )

    def test_complete_blitter_matrix_keeps_required_formats_and_all_rops(self):
        cases = {
            c['id']: c
            for d in self.catalog['domains']
            for r in d['requirements']
            for c in r['cases']
        }
        contract = cases['2d-native.contract-manifest']['parameters']
        required_formats = {'ARGB8888', 'XRGB8888', 'RGB565'}
        self.assertTrue(required_formats <= set(contract['required_formats']))
        rops = contract['required_ROPs']
        self.assertEqual(len(set(rops)), 16)
        for ident in ('2d-native.fill-all-rops', '2d-native.copy-all-rops'):
            self.assertTrue(required_formats <= set(cases[ident]['parameters']['formats']))
            self.assertEqual(set(cases[ident]['parameters']['rops']), set(rops))

    def test_limited_graphics_does_not_depend_on_full_release_domains(self):
        domains = {d['id']: d for d in self.catalog['domains']}
        for ident in ('gles2', '2d-x11'):
            self.assertNotIn('2d-native', domains[ident]['prerequisites'])
        self.assertNotIn('gles2', domains['2d-x11']['prerequisites'])
        self.assertIn('integration.xorg', domains['2d-x11']['required_milestones'])
        for ident in ('vulkan-feasibility', 'encode-feasibility'):
            self.assertEqual(domains[ident]['required_milestones'], [])

    def test_serial_draw_and_second_application_keep_their_acceptance_extent(self):
        cases = {c['id']: c for m in self.catalog['integration_milestones'] for c in m['cases']}
        repeat = cases['integration.repeat-depth']['parameters']
        self.assertGreaterEqual(repeat['numbered_frames'], 256)
        self.assertTrue({(64, 64), (1280, 800)} <= {tuple(s) for s in repeat['sizes']})
        heads = cases['integration.glxheads-acceptance']
        self.assertEqual(heads['required_backend'], 'HARDWARE')
        self.assertGreaterEqual(heads['parameters']['minimum_seconds'], 120)
        for fallback in ('indirect GLX context', 'software rasterizer'):
            self.assertIn(fallback, heads['parameters']['refuse'])

    def test_duplicate_and_nonfinite_json_refused(self):
        for value in ('{"a":1,"a":2}', '{"a":NaN}', '{"a":1e999}'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                CHECKER.parse(value)


class PublicBoundaryTests(unittest.TestCase):
    def test_generated_trees_are_pruned_and_new_sources_checked_without_git(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('build', 'out', '.local', '.ruff_cache', '.cache'):
                path = root / name
                path.mkdir()
                (path / 'local.md').write_text('[not project content](missing.md)')
                (path / 'alias').symlink_to(root / 'missing')
            (root / 'src' / '__pycache__').mkdir(parents=True)
            (root / 'src' / '__pycache__' / 'alias').symlink_to(root / 'missing')
            (root / 'src' / 'build').mkdir()
            (root / 'src' / 'build' / 'source.c').write_text('int value;')
            (root / 'src' / 'new.c').write_text('int other;')
            (root / '.env').write_text('local environment')
            (root / '.env.local').write_text('local environment')
            (root / 'README.md').write_text('project')
            (root / 'src' / 'temporary.o').write_bytes(b'generated')
            # A release archive need not have .git; no Git inventory is consulted.
            scandir = os.scandir

            def project_scandir(path):
                relative = Path(path).relative_to(root)
                self.assertFalse(
                    relative.parts
                    and relative.parts[0] in {'build', 'out', '.local', '.ruff_cache', '.cache'}
                )
                self.assertNotIn('__pycache__', relative.parts)
                return scandir(path)

            with mock.patch.object(os, 'scandir', side_effect=project_scandir):
                selected = list(CHECKER.project_files(root))
            self.assertEqual(
                {relative for _, relative in selected},
                {'README.md', 'src/build/source.c', 'src/new.c'},
            )

    def test_source_symlinks_are_still_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'source').mkdir()
            (root / 'source' / 'alias').symlink_to(root / 'missing')
            with self.assertRaisesRegex(ValueError, 'symlinked project content'):
                list(CHECKER.project_files(root))

    def test_factual_source_attribution_allowed(self):
        CHECKER.scan_text('example.md', 'Linux and Mesa retain their source notices.')

    def test_developer_path_refused(self):
        with self.assertRaises(ValueError):
            CHECKER.scan_text('example.md', '/' + 'home/developer/work/file.c')

    def test_private_key_refused(self):
        with self.assertRaises(ValueError):
            CHECKER.scan_text('example.txt', '-----BEGIN ' + 'OPENSSH PRIVATE KEY-----')

    def test_token_refused(self):
        with self.assertRaises(ValueError):
            CHECKER.scan_text('example.txt', 'gh' + 'p_' + 'a' * 30)

    def test_credential_assignment_refused(self):
        with self.assertRaises(ValueError):
            CHECKER.scan_text('example.txt', 'pass' + 'word = literal')

    def test_local_markdown_link_checks_target_and_escape(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'docs').mkdir()
            (root / 'README.md').write_text('readme')
            CHECKER.markdown_links(root, 'docs/test.md', '[root](../README.md)')
            (root / 'docs/test.md').write_text('# Here\n')
            CHECKER.markdown_links(root, 'docs/test.md', '# Here\n[anchor](#here)')
            CHECKER.markdown_links(root, 'docs/test.md', '[web](https://example.org/doc)')
            with self.assertRaises(ValueError):
                CHECKER.markdown_links(root, 'docs/test.md', '[missing](no-file.md)')
            with self.assertRaises(ValueError):
                CHECKER.markdown_links(root, 'docs/test.md', '[escape](../../outside.md)')

    def test_markdown_anchors_case_duplicates_and_examples(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            text = (
                '# Shared `image`\n# Shared image\n```md\n# Not an anchor\n[x](missing.md)\n```\n'
            )
            (root / 'Source File.md').write_text(text)
            CHECKER.markdown_links(root, 'README.md', '[ok](Source%20File.md#shared-image-1)')
            CHECKER.markdown_links(root, 'Source File.md', text)
            for target in (
                'source%20file.md',
                'Source%20File.md#Shared-image',
                'Source%20File.md#not-an-anchor',
                'file:///tmp/example.md',
            ):
                with self.subTest(target=target), self.assertRaises(ValueError):
                    CHECKER.markdown_links(root, 'README.md', '[bad](' + target + ')')

    def test_html_logo_links_and_images_use_local_link_checks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'logo.svg').write_text('<svg/>')
            (root / 'logo.md').write_text('# Meaning\n')
            CHECKER.markdown_links(
                root,
                'README.md',
                '<a href="logo.md#meaning"><img src="logo.svg#gh-light-mode-only"></a>',
            )
            for target in ('missing.md', 'logo.md#missing', '../outside.md', 'file:///tmp/x'):
                for tag, attribute in (('a', 'href'), ('img', 'src')):
                    with self.subTest(target=target, tag=tag), self.assertRaises(ValueError):
                        CHECKER.markdown_links(root, 'README.md', f'<{tag} {attribute}="{target}">')
            CHECKER.markdown_links(
                root,
                'README.md',
                '```html\n<img src="missing.svg">\n```\n'
                '<!-- <a href="missing.md"> -->\n`<img src="missing.svg">`',
            )

    def test_ordinary_input_cannot_be_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'target').write_text('data')
            (root / 'alias').symlink_to('target')
            with self.assertRaises(ValueError):
                CHECKER.ordinary(root, 'alias')

    def test_duplicate_explicit_anchors_are_refused(self):
        with self.assertRaisesRegex(ValueError, 'duplicate explicit'):
            CHECKER.markdown_anchors('<a id="same"></a>\n<a id="same"></a>\n')
        with self.assertRaisesRegex(ValueError, 'duplicate explicit anchor attribute'):
            CHECKER.markdown_anchors('<a id="first" id="second"></a>')
        self.assertEqual(
            CHECKER.markdown_anchors('```html\n<a id="same"></a>\n```\n<a id="same"></a>'),
            {'same'},
        )

    def test_anchor_attributes_in_examples_or_comments_are_not_targets(self):
        for text in (
            '`id="integration.bootstrap"`',
            'The old field was id="integration.bootstrap".',
            '`<a id="integration.bootstrap"></a>`',
            '<!-- <a id="integration.bootstrap"></a> -->',
            '<a title=\'id="integration.bootstrap"\'></a>',
        ):
            with self.subTest(text=text):
                self.assertNotIn('integration.bootstrap', CHECKER.markdown_anchors(text))
        self.assertIn(
            'integration.bootstrap',
            CHECKER.markdown_anchors('<a id="integration.bootstrap"></a>'),
        )
        self.assertNotIn(
            'acceptance-rules',
            CHECKER.markdown_anchors('<!--\n## Acceptance rules\n-->'),
        )

    def test_document_routes_follow_transitive_links_and_cycles(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'docs/sub').mkdir(parents=True)
            (root / 'README.md').write_text('[agents](AGENTS.md)')
            (root / 'AGENTS.md').write_text('[map](MAP.md) [memory](MEMORY.md)')
            (root / 'MEMORY.md').write_text('[root](README.md)')
            (root / 'MAP.md').write_text('[guide](docs/sub/Guide%20One.md#usage)')
            (root / 'docs/sub/Guide One.md').write_text('# Usage\n[root](../../README.md)')
            CHECKER.documentation_routes(root)

    def test_new_document_requires_both_reader_route_and_owner_map(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'docs').mkdir()
            (root / 'README.md').write_text('[agents](AGENTS.md)')
            (root / 'AGENTS.md').write_text('[map](MAP.md) [memory](MEMORY.md)')
            (root / 'MAP.md').write_text('# Map\n')
            (root / 'MEMORY.md').write_text('# Memory\n')
            (root / 'docs/new.md').write_text('# New\n')
            with self.assertRaisesRegex(ValueError, 'unreachable.*docs/new.md'):
                CHECKER.documentation_routes(root)
            with (root / 'README.md').open('a') as output:
                output.write('\n[new](docs/new.md)')
            with self.assertRaisesRegex(ValueError, 'missing from MAP.*docs/new.md'):
                CHECKER.documentation_routes(root)
            (root / 'MAP.md').write_text('[owner](docs/new.md)')
            CHECKER.documentation_routes(root)

    def test_examples_and_remote_links_do_not_supply_a_document_route(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'docs').mkdir()
            (root / 'README.md').write_text(
                '```md\n[agents](AGENTS.md)\n```\n[remote](https://example.org/AGENTS.md)'
            )
            for name in ('AGENTS.md', 'MAP.md', 'MEMORY.md'):
                (root / name).write_text('# Guide\n')
            with self.assertRaisesRegex(ValueError, 'unreachable.*AGENTS.md'):
                CHECKER.documentation_routes(root)

    def test_authored_docs_are_read_only_and_checked_marks_need_scoped_basis(self):
        import shutil

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'project'
            shutil.copytree(
                ROOT, root, ignore=shutil.ignore_patterns('.git', 'build', '.cache', '__pycache__')
            )
            readme = root / 'README.md'
            authored = '# A directly edited introduction\n\n' + readme.read_text()
            readme.write_text(authored)
            report = CHECKER.check(root)
            self.assertEqual(readme.read_text(), authored)
            self.assertFalse(report['run_results_evaluated'])
            self.assertFalse(report['hardware_capability_promoted'])
            checklist = root / 'docs/checklist.md'
            text = checklist.read_text().replace(
                '- [ ] `integration.bootstrap`', '- [x] `integration.bootstrap`'
            )
            checklist.write_text(text)
            with self.assertRaisesRegex(ValueError, 'reviewed basis'):
                CHECKER.check(root)
            text = text.replace('- [x] `integration.bootstrap`', '- [ ] `integration.bootstrap`')
            # An editorial change must not require regenerating the document.
            checklist.write_text(text + '\nA human-maintained explanation.\n')
            CHECKER.check(root)
            with readme.open('a') as output:
                output.write('\n[broken](missing.md)\n')
            with self.assertRaisesRegex(ValueError, 'broken Markdown link'):
                CHECKER.check(root)

    def test_checked_basis_scope_cannot_substitute_cpu_for_hardware(self):
        import shutil

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'docs').mkdir()
            (root / 'tests').mkdir()
            shutil.copytree(ROOT / 'tests/acceptance', root / 'tests/acceptance')
            text = (ROOT / 'docs/checklist.md').read_text()
            text = text.replace('- [ ] `integration.bootstrap`', '- [x] `integration.bootstrap`')
            lines = text.splitlines()
            i = next(
                i
                for i, line in enumerate(lines)
                if line.startswith('- [x] `integration.bootstrap`')
            )
            lines.insert(i + 1, '  Basis (CPU_VM): [review](review.md)')
            (root / 'docs/review.md').write_text(
                '# Reviewed binding\n\nSource/build/profile: fixture.\n'
            )
            (root / 'docs/checklist.md').write_text('\n'.join(lines) + '\n')
            with self.assertRaisesRegex(ValueError, 'scope mismatch'):
                CHECKER.validate_acceptance_documents(
                    root, CHECKER.parse((ROOT / 'spec/dod.json').read_bytes())
                )
            (root / 'docs/checklist.md').write_text(
                '\n'.join(lines).replace('Basis (CPU_VM)', 'Basis (HARDWARE)') + '\n'
            )
            CHECKER.validate_acceptance_documents(
                root, CHECKER.parse((ROOT / 'spec/dod.json').read_bytes())
            )

    def test_fenced_example_cannot_replace_a_checklist_requirement(self):
        import shutil

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'docs').mkdir()
            shutil.copytree(ROOT / 'tests/acceptance', root / 'tests/acceptance')
            text = (ROOT / 'docs/checklist.md').read_text()
            lines = text.splitlines()
            i = next(
                i
                for i, line in enumerate(lines)
                if line.startswith('- [ ] `integration.bootstrap`')
            )
            for begin, end in (('```md', '```'), ('<!--', '-->')):
                hidden = [*lines[:i], begin, *lines[i : i + 2], end, *lines[i + 2 :]]
                (root / 'docs/checklist.md').write_text('\n'.join(hidden) + '\n')
                with (
                    self.subTest(begin=begin),
                    self.assertRaisesRegex(ValueError, 'lost or unknown checklist'),
                ):
                    CHECKER.validate_acceptance_documents(
                        root, CHECKER.parse((ROOT / 'spec/dod.json').read_bytes())
                    )

    def test_tool_index_covers_commands_and_nested_helpers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'tools/helpers').mkdir(parents=True)
            (root / 'docs').mkdir()
            (root / 'tools/run.py').write_text('')
            (root / 'tools/helpers/inputs.py').write_text('')
            guide = root / 'docs/tools.md'
            guide.write_text('[run](../tools/run.py)\n')
            with self.assertRaisesRegex(ValueError, 'missing.*tools/helpers/inputs.py'):
                CHECKER.tool_documentation(root)
            guide.write_text('[run](../tools/run.py)\n[inputs](../tools/helpers/inputs.py)\n')
            CHECKER.tool_documentation(root)

    def test_tool_index_rejects_deleted_links_and_example_only_coverage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'tools').mkdir()
            (root / 'docs').mkdir()
            tool = root / 'tools/run.py'
            tool.write_text('')
            guide = root / 'docs/tools.md'
            guide.write_text('```md\n[run](../tools/run.py)\n```\n')
            with self.assertRaisesRegex(ValueError, 'missing.*tools/run.py'):
                CHECKER.tool_documentation(root)
            guide.write_text('[run](../tools/run.py)\n')
            tool.unlink()
            with self.assertRaisesRegex(ValueError, 'broken Markdown link'):
                CHECKER.tool_documentation(root)


if __name__ == '__main__':
    unittest.main()
