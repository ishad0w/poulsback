# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Local builder/runner checks. No display, driver, VM or hardware access."""

import argparse
import json
import os
from pathlib import Path
import runpy
import signal
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOL = runpy.run_path(str(Path(__file__).with_name('build.py')))


class BenchmarkTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_catalog_has_unique_cases_and_unchanged_acceptance(self):
        cases = TOOL['cases']()
        self.assertEqual({c['group'] for c in cases.values()}, set(TOOL['GROUPS']))
        self.assertEqual(cases['3D/gears']['binary'], 'glxgears-8.5.0')
        self.assertEqual(cases['3D/heads']['binary'], 'glxheads-8.5.0')
        self.assertEqual(cases['3D/heads']['args'], ['{display}', '{display}'])
        self.assertIn('{size}', cases['3D/gears']['args'])
        self.assertEqual(TOOL['DEFAULT_SIZE'], '640x640')
        self.assertEqual(cases['3D/metrics']['binary'], 'glxgears-metrics')
        for case in cases.values():
            self.assertIsInstance(case['description'], str)
            self.assertNotIn('--run-forever', case['args'])
            self.assertNotIn('-all', case['args'])
            self.assertIn('window', case)
            self.assertEqual('{size}' in case['args'], case['window'] == '{size}')
        for name, case in cases.items():
            if case['binary'].startswith('glmark2'):
                self.assertIn('--benchmark', case['args'], name)

    def test_command_preserves_actual_failure_and_raw_output(self):
        folder = self.root / 'command'
        result = TOOL['command'](
            [
                sys.executable,
                '-c',
                "import sys; print('out'); print('err', file=sys.stderr); sys.exit(7)",
            ],
            folder,
        )
        self.assertEqual(result['state'], 'FAILED')
        self.assertEqual(result['actual_returncode'], 7)
        self.assertEqual((folder / 'stdout.txt').read_text(), 'out\n')
        self.assertEqual((folder / 'stderr.txt').read_text(), 'err\n')
        self.assertEqual(json.loads((folder / 'command.json').read_text()), result)

    def test_spawn_failure_is_recorded(self):
        folder = self.root / 'spawn'
        result = TOOL['command']([str(self.root / 'does-not-exist')], folder)
        self.assertEqual(result['state'], 'SPAWN_FAILED')
        self.assertIsNone(result['actual_returncode'])
        self.assertTrue((folder / 'stdout.txt').exists())
        self.assertTrue((folder / 'stderr.txt').exists())
        self.assertIn('error', json.loads((folder / 'command.json').read_text()))

    def test_timeout_retains_output_and_remains_timeout_after_clean_exit(self):
        result = TOOL['command'](
            [
                sys.executable,
                '-c',
                "import signal,time; signal.signal(signal.SIGINT, lambda *_: exit(0)); "
                "print('before timeout', flush=True); time.sleep(60)",
            ],
            self.root / 'timeout',
            seconds=0.5,
        )
        self.assertEqual(result['state'], 'TIMED_OUT')
        self.assertEqual(result['actual_returncode'], 0)
        self.assertEqual(result['stop_request'], 'SIGINT')
        self.assertIn('before timeout', (self.root / 'timeout/stdout.txt').read_text())

    def test_interactive_observation_stop_is_separate_from_success(self):
        result = TOOL['command'](
            [sys.executable, '-c', 'import time; time.sleep(60)'],
            self.root / 'interactive',
            seconds=0.5,
            scheduled_stop=True,
        )
        self.assertEqual(result['state'], 'STOPPED')
        self.assertEqual(result['actual_returncode'], -signal.SIGINT)
        self.assertEqual(result['scope'], 'APPLICATION_ONLY')

    def test_driver_blocked_child_is_not_killed_or_retried(self):
        class BlockedChild:
            pid = 123456

            def wait(self, timeout):
                raise TOOL['subprocess'].TimeoutExpired('blocked', timeout)

            def poll(self):
                return None

        with patch('subprocess.Popen', return_value=BlockedChild()) as spawn:
            with patch('os.killpg') as kill:
                result = TOOL['command'](['blocked-test'], self.root / 'blocked', seconds=0.1)
        spawn.assert_called_once()
        kill.assert_called_once_with(123456, signal.SIGINT)
        self.assertEqual(result['state'], 'STILL_RUNNING')
        self.assertIsNone(result['actual_returncode'])

    def test_overwriting_results_fails_before_spawn(self):
        folder = self.root / 'exists'
        folder.mkdir()
        with patch('subprocess.Popen') as spawn:
            with self.assertRaises(FileExistsError):
                TOOL['command'](['unused'], folder)
        spawn.assert_not_called()

    def test_output_scope_and_finite_seconds(self):
        with self.assertRaises(ValueError):
            TOOL['local_path'](str(TOOL['ROOT'] / 'generated'))
        for text in ('0', '-1', 'nan', 'inf'):
            with self.assertRaises(argparse.ArgumentTypeError):
                TOOL['positive_seconds'](text)
        self.assertEqual(TOOL['positive_seconds']('120'), 120)

    def test_window_dimensions_and_fixed_size_exceptions(self):
        for invalid in ('0x640', '640x0', '640', '-640x640', '640x640x2', 'nanx640'):
            with self.assertRaises(argparse.ArgumentTypeError):
                TOOL['window_size'](invalid)
        self.assertEqual(TOOL['window_size']('640x640'), '640x640')
        cases = TOOL['cases']()
        self.assertEqual(cases['2D/x11-copy']['window'], '600x600')
        self.assertEqual(cases['3D/heads']['window'], '300x300')
        self.assertEqual(cases['OpenGL/fbo']['window'], '512x512')
        self.assertEqual(cases['OpenGL/glmark2-build']['window'], '{size}')
        self.assertEqual(cases['OpenGL/glmark2-validate']['window'], '800x600')
        self.assertNotIn('--size', cases['OpenGL/glmark2-validate']['args'])

    def test_tampered_binary_is_refused_before_display_or_spawn(self):
        folder = self.root / 'bundle'
        (folder / 'bin/2D').mkdir(parents=True)
        binary = folder / 'bin/2D/rendercheck'
        binary.write_bytes(b'original')
        manifest = {
            'binaries': {
                'bin/2D/rendercheck': {
                    'bytes': binary.stat().st_size,
                    'sha256': TOOL['digest'](binary),
                }
            }
        }
        (folder / 'manifest.json').write_text(json.dumps(manifest))
        binary.write_bytes(b'modified')
        args = argparse.Namespace(case='2D/rendercheck', bundle=str(folder))
        with patch.dict(os.environ, {}, clear=True), patch('subprocess.Popen') as spawn:
            with self.assertRaisesRegex(ValueError, 'differs'):
                TOOL['run_case'](args)
        spawn.assert_not_called()

    def test_bundle_uses_the_root_benchmark_notices_only(self):
        checkout = self.root / 'repo'
        benchmark = checkout / 'benchmark'
        benchmark.mkdir(parents=True)
        (checkout / 'NOTICES.md').write_text(
            '# Notices\n\n## Other component\nUnrelated terms.\n\n'
            '## Benchmark tools\nKeep GPL-3.0-or-later and author attribution.\n'
            '`benchmark/OpenGL/glmark2/`\n\n## Following component\nOther terms.\n'
        )
        with patch.dict(TOOL['bundle_notices'].__globals__, ROOT=benchmark):
            notice = TOOL['bundle_notices']()
        self.assertIn('GPL-3.0-or-later and author attribution', notice)
        self.assertIn('`OpenGL/glmark2/`', notice)
        self.assertNotIn('benchmark/', notice)
        self.assertNotIn('Unrelated terms', notice)
        self.assertNotIn('Following component', notice)

    def test_build_metadata_preserves_old_and_new_meson_compiler_records(self):
        for modern in (False, True):
            with self.subTest(modern=modern):
                work = self.root / str(modern)
                (work / 'meson-info').mkdir(parents=True)
                (work / 'meson-logs').mkdir()
                (work / 'compile_commands.json').write_text('["actual command"]')
                for name in ('intro-dependencies.json', 'intro-buildoptions.json'):
                    (work / 'meson-info' / name).write_text('{}')
                (work / 'meson-logs/meson-log.txt').write_text('actual compiler detection')
                if modern:
                    (work / 'meson-info/intro-compilers.json').write_text(
                        'actual compiler identity'
                    )
                output = work / 'recorded'
                TOOL['copy_build_info'](work, output)
                self.assertEqual(
                    (output / 'compile_commands.json').read_text(), '["actual command"]'
                )
                compiler = output / ('intro-compilers.json' if modern else 'meson-log.txt')
                self.assertIn('actual compiler', compiler.read_text())

    def test_bundle_notices_can_be_reused_from_a_standalone_bundle(self):
        bundle = self.root / 'bundle'
        bundle.mkdir()
        (bundle / 'manifest.json').write_text('{}')
        original = '# Licensing and source notices\n\n## Benchmark tools\nKeep these terms.\n'
        (bundle / 'NOTICES.md').write_text(original)
        (self.root / 'NOTICES.md').write_text('Unrelated parent directory')
        with patch.dict(TOOL['bundle_notices'].__globals__, ROOT=bundle):
            self.assertEqual(TOOL['bundle_notices'](), original)

    def test_missing_root_benchmark_notices_refuses_packaging(self):
        benchmark = self.root / 'benchmark'
        benchmark.mkdir()
        (self.root / 'NOTICES.md').write_text('# Notices\nNo benchmark section.\n')
        with patch.dict(TOOL['bundle_notices'].__globals__, ROOT=benchmark):
            with self.assertRaisesRegex(ValueError, 'missing the benchmark tools section'):
                TOOL['bundle_notices']()


if __name__ == '__main__':
    unittest.main()
