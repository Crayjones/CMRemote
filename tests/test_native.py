import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch, MagicMock
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def runner_module():
    spec = importlib.util.spec_from_file_location('native_runner', ROOT / 'scripts/test-native.py')
    runner = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(ROOT / 'scripts'))
    try: spec.loader.exec_module(runner)
    finally: sys.path.pop(0)
    return runner


class NativeRunner(unittest.TestCase):
    def workspace(self):
        base = ROOT / 'build/macos/test-data' if sys.platform == 'darwin' else ROOT / 'build/linux/test-data'
        base.mkdir(parents=True, exist_ok=True)
        return tempfile.TemporaryDirectory(dir=base)

    def test_native_display_rejects_other_platforms_and_uncontained_output(self):
        runner = runner_module()
        with self.workspace() as directory, patch.object(runner, 'ROOT', Path(directory)):
            for platform, build in [('offscreen', Path(directory) / 'build/linux'), ('xcb', Path(directory).parent)]:
                with self.assertRaises(RuntimeError):
                    with runner.native_display(platform, {}, build): pass

    def test_xcb_owns_display_lifetime_without_changing_caller(self):
        runner = runner_module(); before = dict(os.environ)
        with self.workspace() as directory, patch.object(runner, 'ROOT', Path(directory)):
            child = MagicMock(); child.poll.return_value = None
            def start(command, **kwargs):
                self.assertEqual(command[0], 'Xvfb'); self.assertIn('-nolisten', command)
                self.assertNotIn('-ac', command)
                os.write(int(command[command.index('-displayfd') + 1]), b'42\n')
                return child
            with patch.object(runner.subprocess, 'Popen', side_effect=start):
                with runner.native_display('xcb', {'HOME': before.get('HOME', ''), 'DISPLAY': ':old'}, Path(directory) / 'build/linux') as env:
                    self.assertEqual(env['DISPLAY'], ':42'); self.assertEqual(env['QT_QPA_PLATFORM'], 'xcb')
                    self.assertEqual(env['HOME'], before.get('HOME', ''))
                child.terminate.assert_called_once(); child.wait.assert_called_once()
        self.assertEqual(dict(os.environ), before)

    def test_wayland_owns_private_runtime_and_gl_compositor(self):
        runner = runner_module()
        with self.workspace() as directory, patch.object(runner, 'ROOT', Path(directory)):
            child = MagicMock(); child.poll.return_value = None
            def start(command, **kwargs):
                self.assertEqual(command[0], 'weston'); self.assertIn('--renderer=gl', command)
                runtime = Path(kwargs['env']['XDG_RUNTIME_DIR'])
                self.assertEqual(runtime.stat().st_mode & 0o777, 0o700)
                (runtime / 'wayland-owned').touch()
                return child
            with patch.object(runner.subprocess, 'Popen', side_effect=start):
                with runner.native_display('wayland', {'DISPLAY': ':old'}, Path(directory) / 'build/linux') as env:
                    runtime = Path(env['XDG_RUNTIME_DIR']); self.assertEqual(env['WAYLAND_DISPLAY'], 'wayland-owned')
                    self.assertNotIn('DISPLAY', env); self.assertEqual(env['QT_QPA_PLATFORM'], 'wayland')
                self.assertFalse(runtime.exists()); child.terminate.assert_called_once()

    def test_display_start_failure_still_joins_owned_child(self):
        runner = runner_module()
        with self.workspace() as directory, patch.object(runner, 'ROOT', Path(directory)):
            child = MagicMock(); child.poll.return_value = 1
            with patch.object(runner.subprocess, 'Popen', return_value=child):
                with self.assertRaises(RuntimeError):
                    with runner.native_display('xcb', {}, Path(directory) / 'build/linux'): pass
            child.wait.assert_called_once()

    def test_owned_container_client_keeps_credentials_out_of_arguments(self):
        runner = runner_module(); before = dict(os.environ)
        env = dict(before, LOCAL_UID='501', LOCAL_GID='20')
        with patch.object(runner.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, 'safe', '')) as run:
            result = runner.container_client('rdp_integration', ['savedConnectionThroughShell'], 'xcb',
                ['docker', '--context', 'owned'], env, 'aabbccddeeff', 'arm64',
                'remotemanager-rdp-test-aabbccddeeff-arm64', {'RM_RDP_TEST_PASSWORD': 'owned-secret'})
        created = run.call_args_list[0]; command = created.args[0]
        self.assertIn('create', command)
        self.assertIn('org.remotemanager.instance=aabbccddeeff', command)
        self.assertNotIn('owned-secret', ' '.join(command))
        self.assertIn('container:remotemanager-rdp-test-aabbccddeeff-arm64', command)
        self.assertIn('501:20', command); self.assertIn('rdp_integration', command)
        self.assertEqual(created.kwargs['env']['RM_RDP_TEST_PASSWORD'], 'owned-secret')
        self.assertEqual(created.kwargs['env']['RM_RDP_TEST_PORT'], '3389')
        self.assertEqual(result.stdout, 'safe'); self.assertEqual(dict(os.environ), before)

    def test_sdl_container_keeps_password_only_on_stdin(self):
        runner = runner_module()
        with patch.object(runner.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, 'safe', '')) as run:
            runner.container_client('sdl_comparator', ['/from-stdin:force', '-clipboard'], 'xcb', ['docker'],
                {'LOCAL_UID': '501', 'LOCAL_GID': '20'}, 'aabbccddeeff', 'arm64',
                'remotemanager-performance-aabbccddeeff-abcd1234abcd', {}, stdin='owned-secret\n')
        created, attached = run.call_args_list[:2]
        self.assertIn('--interactive', created.args[0]); self.assertIn('--interactive', attached.args[0])
        self.assertIn('--', created.args[0]); self.assertEqual(attached.kwargs['input'], 'owned-secret\n')
        for call in run.call_args_list:
            self.assertNotIn('owned-secret', ' '.join(call.args[0]))
            self.assertNotIn('RM_RDP_TEST_PASSWORD', call.kwargs['env'])

    def test_sdl_container_rejects_secret_output_and_environment_password(self):
        runner = runner_module()
        args = ('sdl_comparator', ['/from-stdin:force'], 'wayland', ['docker'],
                {'LOCAL_UID': '501', 'LOCAL_GID': '20'}, 'aabbccddeeff', 'arm64',
                'remotemanager-performance-aabbccddeeff-abcd1234abcd')
        with patch.object(runner.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, 'owned-secret', '')):
            with self.assertRaisesRegex(RuntimeError, 'suppressed'):
                runner.container_client(*args, {}, stdin='owned-secret\n')
        with self.assertRaises(ValueError): runner.container_client(*args, {'RM_RDP_TEST_PASSWORD': 'owned-secret'})

    def test_native_sdl_runtime_owns_config_and_preserves_cancel_exit(self):
        import io
        runner = runner_module()
        report = '\n'.join(['RM_PERFORMANCE_RESULT={}', 'RM_COMPARATOR_STOP_REQUESTED=1',
            'RM_COMPARATOR_ABORT_CLEAN=1', 'RM_COMPARATOR_CANCELLED=1', 'RM_COMPARATOR_JOINED=1'])
        with self.workspace() as directory:
            root = Path(directory); prefix = root / '.dev/linux/comparator/prefix'
            (prefix / 'bin').mkdir(parents=True); (prefix / 'bin/sdl-freerdp').touch()
            env = {'QT_QPA_PLATFORM': 'xcb', 'LD_LIBRARY_PATH': '/owned/app/lib', 'RM_RDP_TEST_PASSWORD': 'not-forwarded'}
            with patch.object(runner, 'ROOT', root), patch.object(runner.sys, 'stdin', io.StringIO('owned-secret\n')), \
                 patch.object(runner.subprocess, 'run', return_value=subprocess.CompletedProcess([], 131, report, '')) as run, \
                 patch('builtins.print') as printed:
                self.assertEqual(runner.run_comparator(['/from-stdin:force'], env, root), 131)
            native = run.call_args.kwargs['env']; config = Path(native['RM_COMPARATOR_CONFIG'])
            self.assertTrue(config.is_relative_to(root)); self.assertFalse(config.exists())
            self.assertEqual(native['LD_LIBRARY_PATH'].split(os.pathsep)[0], str(prefix / 'lib'))
            self.assertEqual(native['SDL_VIDEODRIVER'], 'x11'); self.assertNotIn('RM_RDP_TEST_PASSWORD', native)
            self.assertEqual(run.call_args.kwargs['input'], 'owned-secret\n')
            self.assertNotIn('owned-secret', printed.call_args.args[0])
            self.assertIn('RM_NATIVE_EXIT=131', printed.call_args.args[0])
            self.assertEqual(env['LD_LIBRARY_PATH'], '/owned/app/lib')

    def test_sdl_password_line_is_bounded_and_single(self):
        runner = runner_module()
        self.assertTrue(runner.valid_password_line('owned-secret\n'))
        for value in (None, '', '\n', 'secret', 'secret\nextra\n', 'secret\r\n', 'secret\0\n', 'a' * 4096 + '\n'):
            self.assertFalse(runner.valid_password_line(value))

    def test_container_timeout_joins_only_its_created_client(self):
        runner = runner_module(); calls = []
        def execute(command, **kwargs):
            calls.append(command)
            if 'run' in command or 'start' in command: raise subprocess.TimeoutExpired(command, 240)
            return subprocess.CompletedProcess(command, 0, 'safe', '')
        with patch.object(runner.subprocess, 'run', side_effect=execute):
            with self.assertRaises(subprocess.TimeoutExpired):
                runner.container_client('performance_client', ['ownedWorkload'], 'xcb', ['docker'],
                    {'LOCAL_UID': '501', 'LOCAL_GID': '20'}, 'aabbccddeeff', 'arm64',
                    'remotemanager-performance-aabbccddeeff-abcd1234abcd', {})
        removals = [command for command in calls if 'rm' in command]
        self.assertEqual(len(removals), 1)
        self.assertTrue(removals[0][-1].startswith('remotemanager-native-client-aabbccddeeff-'))
        self.assertNotIn('remotemanager-performance-aabbccddeeff-abcd1234abcd', removals[0])

    def test_container_creation_failure_does_not_remove_an_existing_client(self):
        runner = runner_module()
        with patch.object(runner.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, '', 'owned creation failure')) as run:
            with self.assertRaisesRegex(RuntimeError, 'could not be created'):
                runner.container_client('performance_client', ['ownedWorkload'], 'xcb', ['docker'],
                    {'LOCAL_UID': '501', 'LOCAL_GID': '20'}, 'aabbccddeeff', 'arm64',
                    'remotemanager-performance-aabbccddeeff-abcd1234abcd', {})
        self.assertEqual(run.call_count, 1); self.assertIn('create', run.call_args.args[0])

    def test_container_client_rejects_unowned_fixture_and_platform(self):
        runner = runner_module()
        with patch.object(runner.subprocess, 'run') as run:
            for platform, fixture in [('offscreen', 'remotemanager-rdp-test-aabbccddeeff-arm64'),
                                      ('xcb', 'unrelated-fixture')]:
                with self.assertRaises(ValueError):
                    runner.container_client('rdp_integration', [], platform, ['docker'], {},
                        'aabbccddeeff', 'arm64', fixture, {})
            run.assert_not_called()

    def test_container_client_suppresses_secret_output(self):
        runner = runner_module()
        with patch.object(runner.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, 'owned-secret', '')):
            with self.assertRaisesRegex(RuntimeError, 'suppressed'):
                runner.container_client('performance_client', ['ownedWorkload'], 'wayland', ['docker'],
                    {'LOCAL_UID': '501', 'LOCAL_GID': '20'}, 'aabbccddeeff', 'arm64',
                    'remotemanager-performance-aabbccddeeff-abcd1234abcd', {'RM_RDP_TEST_PASSWORD': 'owned-secret'})

    def test_native_runner_includes_owned_rdp_and_performance_targets(self):
        runner = runner_module()
        self.assertIn('rdp_integration', runner.TARGETS); self.assertIn('performance_client', runner.TARGETS)
        self.assertIn('rdp_session_integration', runner.TARGETS)

    def test_native_client_contains_qt_temporary_files(self):
        runner = runner_module()
        with self.workspace() as directory:
            build = Path(directory) / 'build/linux/arm64'; (build / 'app').mkdir(parents=True)
            (build / 'app/qt_smoke').write_text('owned executable placeholder')
            display = MagicMock(); display.__enter__.side_effect = lambda: display_env.copy()
            display_env = {}
            def owned_display(platform, env, root):
                display_env.update(env); return display
            result = subprocess.CompletedProcess([], 0, 'Totals: 3 passed, 0 failed, 0 skipped, 10ms\n********* Finished testing of Smoke *********\n', '')
            with patch('sys.argv', ['test-native.py', '--platform', 'xcb', 'qt_smoke']), \
                 patch.object(runner.sys, 'platform', 'linux'), \
                 patch.object(runner, 'environment', return_value={'RM_BUILD_DIR': str(build)}), \
                 patch.object(runner, 'native_display', side_effect=owned_display), \
                 patch.object(runner.subprocess, 'run', return_value=result) as run:
                self.assertEqual(runner.main(), 0)
            for key in ('TMPDIR', 'TMP', 'TEMP'):
                self.assertEqual(Path(run.call_args.kwargs['env'][key]), build / 'test-data')
                self.assertTrue(Path(run.call_args.kwargs['env'][key]).is_dir())

    def test_native_report_rejects_null_rhi_leaks_and_secret_output(self):
        runner = runner_module()
        report = 'Totals: 3 passed, 0 failed, 0 skipped, 10ms\n********* Finished testing of Smoke *********\n'
        self.assertTrue(runner.accepted_report(report))
        for suffix in ('QRhiWidget: No QRhi', 'QRhiWidget: QRhi is not supported', 'unreleased resources'):
            self.assertFalse(runner.accepted_report(report + suffix))
        with self.assertRaisesRegex(RuntimeError, 'suppressed'):
            runner.checked_output(report + 'owned-secret', ('owned-secret',))
        self.assertFalse(runner.accepted_report(report.replace('0 failed', '1 failed')))
