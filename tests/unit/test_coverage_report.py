# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""The coverage ledger cannot reach 100 % while a real gap is left."""

import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('coverage_report', ROOT / 'tools/coverage-report.py')
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)

RUNNER = dict(
    entrypoint='tools/run.py', arguments=[], backend='CPU_VM', repository='driver', builder='C3'
)


def case(ident, case_list=None, bound=None):
    parameters = {'bound': bound} if bound else {'rule': 'x'}
    return dict(
        id=ident,
        required_backend='CPU_VM',
        parameters=parameters,
        artifacts=['cases.jsonl'],
        runner_suite=copy.deepcopy(RUNNER),
        criteria_ref=f'tests/acceptance/a.md#{ident}',
        case_list=case_list,
    )


class LedgerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        (self.root / 'spec/cases').mkdir(parents=True)
        (self.root / 'tools').mkdir()
        (self.root / 'tools/run.py').write_text('')
        (self.root / 'kernel').mkdir()
        (self.root / 'kernel/a.c').write_text('int a;\n')
        cases = [
            case('d.one'),
            case('d.listed', 'spec/cases/d.listed.json'),
            case('d.fast', bound={'min': 10, 'unit': 'FPS'}),
        ]
        self.catalog = dict(
            schema='poulsback-dod-catalog-v4',
            coverage_policy=dict(
                optional_domains=[],
                assessment_domains=[],
                criteria_ref='docs/checklist.md#acceptance-rules',
                m1_domains=['d'],
                after_m1_domains=[],
                exclusions_ref='spec/exclusions.json',
            ),
            required_for_release=['d'],
            domains=[
                dict(
                    id='d',
                    prerequisites=[],
                    required_milestones=[],
                    requirements=[
                        dict(
                            id='d.r',
                            mandatory=True,
                            required_backend='CPU_VM',
                            cases=cases,
                            criteria_ref='tests/acceptance/a.md#d.r',
                        )
                    ],
                    criteria_ref='tests/acceptance/a.md#d',
                )
            ],
            integration_milestones=[],
        )
        self.listing = dict(
            schema='poulsback-case-list-v1',
            family='d.listed',
            source='test',
            criteria='passes',
            runner=None,
            external=None,
            cases=[{'id': 'a'}, {'id': 'b/c'}],
        )
        self.exclusions = dict(schema='poulsback-exclusions-v1', exclusions=[])
        self.write()
        self.git('init', '-q')
        self.git('add', '.')
        self.git('-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-q', '-m', 'one')
        self.commit = self.git('rev-parse', 'HEAD')

    def tearDown(self):
        self.directory.cleanup()

    def git(self, *args):
        out = subprocess.run(
            ['git', '-C', str(self.root), *args], check=True, capture_output=True, text=True
        )
        return out.stdout.strip()

    def write(self):
        (self.root / 'spec/dod.json').write_text(json.dumps(self.catalog))
        (self.root / 'spec/cases/d.listed.json').write_text(json.dumps(self.listing))
        (self.root / 'spec/exclusions.json').write_text(json.dumps(self.exclusions))

    def results(self, rows, **common):
        path = self.root / 'cases.jsonl'
        base = dict(
            schema='poulsback-case-result-v1',
            commit=self.commit,
            tree='clean',
            run='r',
            verdict='pass',
            **{'class': 'HOST'},
        )
        path.write_text(''.join(json.dumps(dict(base, **common, **r)) + '\n' for r in rows))
        return [path]

    def run_report(self, rows=(), **common):
        self.write()
        ledger = REPORT.Ledger(self.root, None)
        REPORT.check(ledger)
        files = self.results(list(rows), **common)
        return REPORT.report(ledger, files, 'HEAD', self.root)

    def all_pass(self):
        return [
            {'case': 'd.one'},
            {'case': 'd.listed/a'},
            {'case': 'd.listed/b/c'},
            {'case': 'd.fast', 'value': 12, 'unit': 'FPS'},
        ]

    def test_every_case_passed_is_complete(self):
        data = self.run_report(self.all_pass())
        self.assertEqual(data['m1']['cases'], 4)
        self.assertEqual(data['m1']['passed'], 4)
        self.assertTrue(data['m1_complete'])

    def test_missing_result_dirty_tree_or_other_product_is_not_a_pass(self):
        rows = self.all_pass()[1:]
        self.assertFalse(self.run_report(rows)['m1_complete'])
        self.assertEqual(self.run_report(self.all_pass(), tree='dirty')['m1']['run'], 0)
        (self.root / 'kernel/a.c').write_text('int b;\n')
        self.git('-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-q', '-am', 'two')
        data = self.run_report(self.all_pass())
        self.assertEqual(data['m1']['passed'], 0)

    def test_a_docs_only_commit_keeps_the_results(self):
        (self.root / 'notes.md').write_text('docs\n')
        self.git('add', 'notes.md')
        self.git('-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-q', '-m', 'docs')
        self.assertTrue(self.run_report(self.all_pass())['m1_complete'])

    def test_skip_xfail_flaky_and_unmet_bound_are_not_passes(self):
        for verdict in ('skip', 'xfail', 'fail'):
            rows = self.all_pass() + [{'case': 'd.one', 'verdict': verdict}]
            with self.subTest(verdict=verdict):
                self.assertEqual(self.run_report(rows)['m1']['passed'], 3)
        rows = self.all_pass()[:3] + [{'case': 'd.fast', 'value': 9, 'unit': 'FPS'}]
        self.assertEqual(self.run_report(rows)['m1']['passed'], 3)
        rows = self.all_pass()[:3] + [{'case': 'd.fast'}]
        self.assertEqual(self.run_report(rows)['m1']['passed'], 3)

    def test_nonfinite_measurements_and_bounds_are_refused(self):
        # NaN compares false both ways, so it once passed a minimum.
        for value in (float('nan'), float('inf'), float('-inf')):
            rows = self.all_pass()[:3] + [{'case': 'd.fast', 'value': value, 'unit': 'FPS'}]
            with self.subTest(value=value):
                data = self.run_report(rows)
                self.assertEqual(data['m1']['passed'], 3)
                self.assertFalse(data['m1_complete'])
                self.assertTrue(any('non-finite' in p for p in data['problems']), data['problems'])
        bound = self.catalog['domains'][0]['requirements'][0]['cases'][2]['parameters']['bound']
        for limit in (float('nan'), float('inf')):
            bound['min'] = limit
            self.write()
            with self.subTest(limit=limit), self.assertRaisesRegex(ValueError, 'non-finite'):
                REPORT.Ledger(self.root, None)

    def test_planned_runner_counts_no_runner_and_no_pass(self):
        self.catalog['domains'][0]['requirements'][0]['cases'][0]['runner_suite']['entrypoint'] = (
            'tools/later.py'
        )
        data = self.run_report(self.all_pass())
        self.assertEqual(data['m1']['with_runner'], 3)
        self.assertEqual(data['m1']['passed'], 3)
        self.assertFalse(data['m1_complete'])
        self.assertIn('C3 | tools/later.py', data['missing_runners'])

    def test_exclusion_is_apart_and_needs_a_maintainer(self):
        entry = dict(
            match='exact',
            pattern='d.one',
            reason='does not apply',
            evidence='spec says so',
            accepted_by='agent',
            accepted_on='2026-11-03',
        )
        self.exclusions['exclusions'] = [entry]
        self.write()
        with self.assertRaisesRegex(ValueError, 'accepted by a maintainer'):
            REPORT.Ledger(self.root, None)
        entry['accepted_by'] = 'maintainer'
        data = self.run_report(self.all_pass()[1:])
        self.assertEqual((data['m1']['passed'], data['m1']['excluded']), (3, 1))
        self.assertTrue(data['m1_complete'])

    def test_unenumerated_external_list_blocks_completion(self):
        self.listing['cases'] = []
        self.listing['external'] = dict(
            source='suite/list.txt',
            member=None,
            select=['*'],
            count=None,
            sha256=None,
            source_sha256=None,
            builder='C4',
            note='later',
        )
        data = self.run_report(self.all_pass())
        self.assertEqual(data['m1']['not_enumerated'], 1)
        self.assertFalse(data['m1_complete'])

    def test_pinned_external_list_must_be_passed_whole(self):
        names = ['x', 'y']
        self.listing['cases'] = []
        self.listing['external'] = dict(
            source='suite/list.txt',
            member=None,
            select=['*'],
            count=2,
            sha256=REPORT.list_sha256(names),
            source_sha256=REPORT.list_sha256(names),
            builder='C4',
            note='pinned',
        )
        pin = REPORT.list_sha256(names)
        rows = [r for r in self.all_pass() if not r['case'].startswith('d.listed')]
        partial = rows + [{'case': 'd.listed/x', 'list_sha256': pin}]
        self.assertFalse(self.run_report(partial)['m1_complete'])
        whole = partial + [{'case': 'd.listed/y', 'list_sha256': pin}]
        self.assertTrue(self.run_report(whole)['m1_complete'])
        wrong = partial + [{'case': 'd.listed/z', 'list_sha256': pin}]
        data = self.run_report(wrong)
        self.assertFalse(data['m1_complete'])
        self.assertTrue(data['problems'])

    def external(self, names, select=('*',)):
        return dict(
            source='suite/list.txt',
            member=None,
            select=list(select),
            count=len(names),
            sha256=REPORT.list_sha256(names),
            source_sha256=REPORT.list_sha256(names),
            builder='C4',
            note='pinned',
        )

    def test_listed_results_count_only_from_the_pinned_list(self):
        names = ['x', 'y']
        self.listing['cases'] = []
        self.listing['external'] = self.external(names)
        pin = self.listing['external']['source_sha256']
        rows = [r for r in self.all_pass() if not r['case'].startswith('d.listed')]
        listed = [{'case': 'd.listed/x'}, {'case': 'd.listed/y'}]
        self.assertFalse(self.run_report(rows + listed)['m1_complete'])
        other = [dict(r, list_sha256='0' * 64) for r in listed]
        self.assertFalse(self.run_report(rows + other)['m1_complete'])
        pinned = [dict(r, list_sha256=pin) for r in listed]
        self.assertTrue(self.run_report(rows + pinned)['m1_complete'])

    def test_derived_case_needs_every_match_and_at_least_one(self):
        self.catalog['domains'][0]['requirements'][0]['cases'].append(
            case('d.view', 'spec/cases/d.view.json')
        )
        view = dict(self.listing, family='d.view', cases=[
            {'id': 'some', 'derive': {'family': 'd.listed', 'match': ['a*']}},
            {'id': 'none', 'derive': {'family': 'd.listed', 'match': ['zz*']}},
        ])
        names = ['a1', 'a2', 'b']
        self.listing['cases'] = []
        self.listing['external'] = self.external(names)
        pin = self.listing['external']['source_sha256']
        (self.root / 'spec/cases/d.view.json').write_text(json.dumps(view))
        rows = [r for r in self.all_pass() if not r['case'].startswith('d.listed')]
        listed = [{'case': f'd.listed/{n}', 'list_sha256': pin} for n in names]
        data = self.run_report(rows + listed)
        self.assertEqual(data['m1']['cases'], 2 + 3 + 2)
        self.assertIn(('d.view/none', 'fail'), data['failures'])
        self.assertNotIn('d.view/some', [f[0] for f in data['failures']])
        broken = listed[:1] + [dict(listed[1], verdict='fail')] + listed[2:]
        data = self.run_report(rows + broken)
        self.assertIn(('d.view/some', 'fail'), data['failures'])

    def test_ids_maps_listed_names_and_refuses_others(self):
        names = ['x', 'y']
        self.listing['cases'] = []
        self.listing['external'] = self.external(names)
        (self.root / 'suite').mkdir()
        (self.root / 'suite/list.txt').write_text('x\ny\n')
        self.write()
        ledger = REPORT.Ledger(self.root, self.root)
        ids = REPORT.case_ids(ledger, 'suite/list.txt', None, ['y', 'z'])
        self.assertEqual(ids, ['d.listed/y', None])

    def test_retired_listed_case_is_kept_and_not_counted(self):
        self.listing['cases'].append({'id': 'old', 'retired': 'replaced by d.one'})
        self.assertTrue(self.run_report(self.all_pass())['m1_complete'])
        self.listing['cases'][-1]['retired'] = ''
        with self.assertRaises(ValueError):
            self.run_report(self.all_pass())

    def test_check_refuses_a_case_without_runner_or_criteria(self):
        self.catalog['domains'][0]['requirements'][0]['cases'][0]['runner_suite'] = None
        self.write()
        with self.assertRaisesRegex(ValueError, 'without a runner'):
            REPORT.check(REPORT.Ledger(self.root, None))
        self.tearDown()
        self.setUp()
        self.listing['criteria'] = None
        self.catalog['domains'][0]['requirements'][0]['cases'][1]['criteria_ref'] = ''
        self.write()
        with self.assertRaises(ValueError):
            REPORT.check(REPORT.Ledger(self.root, None))

    def test_unreferenced_case_list_is_refused(self):
        (self.root / 'spec/cases/stray.json').write_text('{}')
        with self.assertRaisesRegex(ValueError, 'unreferenced case lists'):
            REPORT.check(REPORT.Ledger(self.root, None))


if __name__ == '__main__':
    unittest.main()
