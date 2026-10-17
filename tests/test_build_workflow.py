import importlib.util
import json
import os
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch


ROOT = Path(__file__).resolve().parents[1]
VERSIONS = json.loads((ROOT / 'dependencies.lock.json').read_text())['versions']
CMAKE_LAYOUTS = {
    'macos': 'cmake-{version}-macos-universal/CMake.app/Contents/bin/cmake',
    'linux-arm64': 'cmake-{version}-linux-aarch64/bin/cmake',
    'linux-amd64': 'cmake-{version}-linux-x86_64/bin/cmake',
}


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / (name + '.py'))
    value = importlib.util.module_from_spec(spec)
    if name == 'project':
        with patch.dict(sys.modules, {'env': module('env')}):
            spec.loader.exec_module(value)
    else:
        spec.loader.exec_module(value)
    return value


class BuildWorkflow(unittest.TestCase):
    def setUp(self):
        base = ROOT / '.dev'
        base.mkdir(exist_ok=True)
        self.workspace = tempfile.TemporaryDirectory(prefix='build-workflow-', dir=base)
        self.addCleanup(self.workspace.cleanup)
        self.root = Path(self.workspace.name)

    def executable(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('owned fixture; never execute')
        path.chmod(0o755)
        return path

    def cmake(self, tools, target, version=None):
        return tools / 'cmake' / CMAKE_LAYOUTS[target].format(version=version or VERSIONS['cmake'])

    def invoke_project(self, arguments, host='macos', failure=None):
        project = module('project')
        build = self.root / 'build' / host
        if host == 'linux':
            build /= 'arm64'
        env = {'RM_BUILD_DIR': str(build), 'RM_PLATFORM': host,
               'RM_APP_BUILD_DIR': str(self.root / 'stale-output')}
        calls = []
        def run(command, **kwargs):
            calls.append((command, dict(kwargs['env'])))
            self.assertEqual(kwargs['cwd'], self.root)
            self.assertTrue(kwargs['check'])
            if len(calls) == failure:
                raise subprocess.CalledProcessError(1, command)
        output = StringIO()
        self.notice_fetches = []
        with patch.object(project, 'ROOT', self.root), patch.object(project, 'environment', return_value=env), \
             patch.object(project, 'fetch_notices', side_effect=self.notice_fetches.append), \
             patch.object(project.os, 'cpu_count', return_value=16), \
             patch.object(project.subprocess, 'run', side_effect=run), \
             patch.object(sys, 'argv', ['project.py', *arguments]), redirect_stdout(output):
            if failure:
                with self.assertRaises(subprocess.CalledProcessError):
                    project.main()
            else:
                project.main()
        return build, calls, output.getvalue()

    def assert_refresh(self, calls, build, mock_only=False):
        self.assertEqual(calls[0][0], ['cmake', '-S', str(self.root), '-B', str(build), '-G', 'Ninja',
                                     '-DCMAKE_BUILD_TYPE=Debug', '-DRM_WITH_RDP=' + ('OFF' if mock_only else 'ON')])
        self.assertEqual(calls[1][0], ['cmake', '--build', str(build), '--parallel', '8'])
        for _, env in calls:
            self.assertEqual(env['RM_APP_BUILD_DIR'], str(build))

    def test_run_refreshes_then_launches_selected_native_or_mock_output(self):
        for host in ('macos', 'linux'):
            for mock_only in (False, True):
                with self.subTest(host=host, mock_only=mock_only):
                    arguments = ['run', '--smoke', '--demo'] + (['--mock-only'] if mock_only else [])
                    base, calls, output = self.invoke_project(arguments, host)
                    build = base / ('app-mock' if mock_only else 'app')
                    self.assertEqual(len(calls), 3)
                    self.assert_refresh(calls, build, mock_only)
                    binary = build / ('RemoteManager.app/Contents/MacOS/RemoteManager' if host == 'macos' else 'RemoteManager')
                    self.assertEqual(calls[2][0], [str(binary), '--dev-root', str(self.root), '--smoke', '--demo'])
                    self.assertIn(str(build), output)

    def test_package_refreshes_then_forwards_selected_output_and_artifact_name(self):
        for host in ('macos', 'linux'):
            for mock_only in (False, True):
                with self.subTest(host=host, mock_only=mock_only):
                    arguments = ['package', '--artifact-name', 'CurrentSource'] + (['--mock-only'] if mock_only else [])
                    base, calls, _ = self.invoke_project(arguments, host)
                    build = base / ('app-mock' if mock_only else 'app')
                    self.assertEqual(len(calls), 4)
                    self.assert_refresh(calls, build, mock_only)
                    self.assertEqual(calls[2][0], ['cargo', 'fetch', '--locked'])
                    self.assertEqual(self.notice_fetches, [host])
                    self.assertEqual(calls[3][0], [sys.executable, str(self.root / 'scripts/package.py'),
                                                  '--artifact-name', 'CurrentSource'])

    def test_refresh_failure_blocks_launch_and_packaging(self):
        for command in ('run', 'package'):
            for failure in (1, 2):
                for mock_only in (False, True):
                    with self.subTest(command=command, failure=failure, mock_only=mock_only):
                        arguments = [command] + (['--mock-only'] if mock_only else [])
                        _, calls, _ = self.invoke_project(arguments, failure=failure)
                        self.assertEqual(len(calls), failure)
                        self.assertTrue(all(command[0] == 'cmake' for command, _ in calls))

    def test_notice_cache_fetch_failure_blocks_packaging(self):
        _, calls, _ = self.invoke_project(['package'], failure=3)
        self.assertEqual(len(calls), 3)
        self.assertEqual(calls[-1][0], ['cargo', 'fetch', '--locked'])
        self.assertEqual(self.notice_fetches, [])
        self.assertFalse(any(str(self.root / 'scripts/package.py') in call[0] for call in calls))

    def test_build_preserves_selected_cmake_targets(self):
        base, calls, _ = self.invoke_project(['build', '--mock-only', '--target', 'qt_smoke', '--target', 'xml_storage_test'])
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[1][0], ['cmake', '--build', str(base / 'app-mock'), '--parallel', '8',
                                     '--target', 'qt_smoke', 'xml_storage_test'])
        self.assertIn('-DRM_WITH_RDP=OFF', calls[0][0])

    def test_build_and_test_keep_full_refresh_before_test_commands(self):
        for command in ('build', 'test'):
            with self.subTest(command=command):
                base, calls, _ = self.invoke_project([command])
                self.assert_refresh(calls, base / 'app')
                if command == 'build':
                    self.assertEqual(len(calls), 2)
                else:
                    self.assertEqual(len(calls), 5)
                    self.assertEqual(calls[2][0], ['cargo', 'test', '--locked'])
                    self.assertEqual(calls[3][0], [sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_*.py', '-v'])
                    self.assertEqual(calls[4][0], ['ctest', '--test-dir', str(base / 'app'), '--output-on-failure'])
                    for _, env in calls[2:]:
                        for name in ('TMPDIR', 'TMP', 'TEMP'):
                            self.assertEqual(env[name], str(base / 'test-data'))

    def test_invalid_targets_fail_before_environment_or_build(self):
        project = module('project')
        arguments = (['run', '--target', 'RemoteManager'], ['package', '--target', 'RemoteManager'],
                     ['test', '--target', 'qt_smoke'], ['build', '--target=--clean-first'],
                     ['build', '--target', '../outside'], ['build', '--target', ''])
        for values in arguments:
            with self.subTest(arguments=values), patch.object(sys, 'argv', ['project.py', *values]), \
                 patch.object(project, 'environment') as environment, \
                 patch.object(project.subprocess, 'run') as run, redirect_stderr(StringIO()):
                with self.assertRaises(SystemExit):
                    project.main()
                environment.assert_not_called()
                run.assert_not_called()

    def sdk(self, target, override=False):
        (self.root / 'dependencies.lock.json').write_text(json.dumps({'versions': VERSIONS}))
        host = 'macos' if target == 'macos' else 'linux'
        sdk = self.root / ('selected-sdk' if override else '.dev/' + host)
        (sdk / 'qt').mkdir(parents=True, exist_ok=True)
        self.executable(self.cmake(sdk / 'tools', target))
        self.executable(sdk / 'tools/bin/ninja')
        return sdk

    def host_patches(self, target):
        return ('Darwin', 'arm64') if target == 'macos' else ('Linux', 'aarch64' if target == 'linux-arm64' else 'x86_64')

    def test_environment_selects_exact_locked_cmake_for_each_platform(self):
        environment = module('env')
        for target in CMAKE_LAYOUTS:
            with self.subTest(target=target):
                sdk = self.sdk(target)
                for old in ('3.99.0', '4.99.0'):
                    self.executable(self.cmake(sdk / 'tools', target, old))
                host, arch = self.host_patches(target)
                before = dict(os.environ)
                with patch.object(environment, 'ROOT', self.root), \
                     patch.object(environment.platform, 'system', return_value=host), \
                     patch.object(environment.platform, 'machine', return_value=arch), \
                     patch.dict(os.environ, {'PATH': '/system/bin', 'HOME': 'unchanged'}, clear=True), \
                     patch.object(environment.subprocess, 'check_output', return_value=VERSIONS['ninja'] + '\n') as version:
                    env = environment.environment()
                    self.assertEqual(dict(os.environ), {'PATH': '/system/bin', 'HOME': 'unchanged'})
                self.assertEqual(dict(os.environ), before)
                self.assertEqual(env['PATH'].split(os.pathsep)[:3],
                                 [str(sdk / 'cargo/bin'), str(self.cmake(sdk / 'tools', target).parent), str(sdk / 'tools/bin')])
                self.assertEqual(env['HOME'], 'unchanged')
                version.assert_called_once_with([str(sdk / 'tools/bin/ninja'), '--version'], text=True, stderr=subprocess.STDOUT)
                expected = self.root / ('build/macos' if target == 'macos' else 'build/linux/' + target.removeprefix('linux-'))
                self.assertEqual(env['RM_BUILD_DIR'], str(expected))

    def test_environment_honors_selected_sdk_root(self):
        environment = module('env')
        sdk = self.sdk('macos', override=True)
        with patch.object(environment, 'ROOT', self.root), \
             patch.object(environment.platform, 'system', return_value='Darwin'), \
             patch.dict(os.environ, {'RM_SDK_ROOT': str(sdk)}), \
             patch.object(environment.subprocess, 'check_output', return_value=VERSIONS['ninja']):
            env = environment.environment()
        self.assertEqual(env['PATH'].split(os.pathsep)[1], str(self.cmake(sdk / 'tools', 'macos').parent))
        self.assertEqual(env['RM_QT_PREFIX'], str(sdk / 'qt'))

    def test_environment_rejects_missing_or_nonexecutable_pin_despite_old_cmake(self):
        environment = module('env')
        for target in CMAKE_LAYOUTS:
            for missing in (False, True):
                with self.subTest(target=target, missing=missing):
                    sdk = self.sdk(target)
                    cmake = self.cmake(sdk / 'tools', target)
                    self.executable(self.cmake(sdk / 'tools', target, '3.99.0'))
                    if missing:
                        cmake.unlink()
                    else:
                        cmake.chmod(0o644)
                    host, arch = self.host_patches(target)
                    with patch.object(environment, 'ROOT', self.root), \
                         patch.object(environment.platform, 'system', return_value=host), \
                         patch.object(environment.platform, 'machine', return_value=arch), \
                         patch.dict(os.environ, {'PATH': '/system/bin'}, clear=True), \
                         patch.object(environment.subprocess, 'check_output') as version:
                        with self.assertRaisesRegex(SystemExit, 'pinned CMake ' + VERSIONS['cmake']):
                            environment.environment()
                    version.assert_not_called()

    def test_environment_rejects_stale_missing_or_broken_local_ninja(self):
        environment = module('env')
        sdk = self.sdk('macos')
        failures = ['1.12.1\n', FileNotFoundError('missing'), subprocess.CalledProcessError(1, ['ninja'])]
        for failure in failures:
            with self.subTest(failure=failure), patch.object(environment, 'ROOT', self.root), \
                 patch.object(environment.platform, 'system', return_value='Darwin'), \
                 patch.dict(os.environ, {'PATH': '/system/bin'}, clear=True), \
                 patch.object(environment.subprocess, 'check_output') as version:
                if isinstance(failure, Exception):
                    version.side_effect = failure
                else:
                    version.return_value = failure
                with self.assertRaisesRegex(SystemExit, 'pinned Ninja ' + VERSIONS['ninja']):
                    environment.environment()
                self.assertEqual(version.call_args.args[0], [str(sdk / 'tools/bin/ninja'), '--version'])

    def test_bootstrap_caches_every_locked_rust_notice(self):
        bootstrap = module('bootstrap')
        artifacts = json.loads((ROOT / 'dependencies.lock.json').read_text())['artifacts']
        artifact = MagicMock()
        bootstrap.fetch_rust_notices(artifacts, artifact)
        fetched = {call.args[0] for call in artifact.call_args_list}
        self.assertEqual(fetched, {key for key in artifacts if key.startswith('rust-notice-')})
        self.assertTrue(fetched)

    def test_bootstrap_installs_exact_cmake_even_with_existing_old_versions(self):
        bootstrap = module('bootstrap')
        for target in CMAKE_LAYOUTS:
            with self.subTest(target=target):
                tools = self.root / target / 'tools'
                old = self.executable(self.cmake(tools, target, '3.99.0'))
                self.executable(tools / 'bin/ninja')
                cmake = self.cmake(tools, target)
                archive = self.root / ('cmake-' + target + '.tar.gz')
                artifact = MagicMock(return_value=archive)
                with patch.object(bootstrap, 'unpack', side_effect=lambda *_: self.executable(cmake)) as unpack, \
                     patch.object(bootstrap.subprocess, 'check_output', return_value=VERSIONS['ninja']):
                    self.assertEqual(bootstrap.install_build_tools(tools, target, VERSIONS, artifact), cmake)
                artifact.assert_called_once_with('cmake-' + target)
                unpack.assert_called_once_with(archive, tools / 'cmake')
                self.assertTrue(old.exists())

    def test_bootstrap_skips_current_tools(self):
        bootstrap = module('bootstrap')
        tools = self.root / 'tools'
        cmake = self.executable(self.cmake(tools, 'macos'))
        self.executable(tools / 'bin/ninja')
        artifact = MagicMock()
        with patch.object(bootstrap, 'unpack') as unpack, \
             patch.object(bootstrap.subprocess, 'check_output', return_value=VERSIONS['ninja'] + '\n'):
            self.assertEqual(bootstrap.install_build_tools(tools, 'macos', VERSIONS, artifact), cmake)
        artifact.assert_not_called()
        unpack.assert_not_called()

    def test_bootstrap_reinstalls_stale_missing_or_broken_ninja(self):
        bootstrap = module('bootstrap')
        tools = self.root / 'tools'
        self.executable(self.cmake(tools, 'macos'))
        failures = ['1.12.1\n', FileNotFoundError('missing'), subprocess.CalledProcessError(1, ['ninja'])]
        for failure in failures:
            with self.subTest(failure=failure):
                ninja = tools / 'bin/ninja'
                artifact = MagicMock(return_value=self.root / 'ninja-mac.zip')
                with patch.object(bootstrap, 'unpack', side_effect=lambda *_: self.executable(ninja)) as unpack, \
                     patch.object(bootstrap.subprocess, 'check_output', side_effect=[failure, VERSIONS['ninja']]) as version:
                    bootstrap.install_build_tools(tools, 'macos', VERSIONS, artifact)
                artifact.assert_called_once_with('ninja-macos')
                unpack.assert_called_once_with(self.root / 'ninja-mac.zip', tools / 'bin')
                self.assertEqual(version.call_count, 2)
                self.assertTrue(os.access(ninja, os.X_OK))

    def test_bootstrap_rejects_archive_without_exact_cmake(self):
        bootstrap = module('bootstrap')
        tools = self.root / 'tools'
        self.executable(self.cmake(tools, 'macos', '3.99.0'))
        with patch.object(bootstrap, 'unpack'), patch.object(bootstrap.subprocess, 'check_output') as version:
            with self.assertRaisesRegex(RuntimeError, 'Pinned CMake .* missing after installation'):
                bootstrap.install_build_tools(tools, 'macos', VERSIONS, MagicMock())
        version.assert_not_called()

    def test_bootstrap_rejects_installed_ninja_with_wrong_version(self):
        bootstrap = module('bootstrap')
        tools = self.root / 'tools'
        self.executable(self.cmake(tools, 'macos'))
        ninja = self.executable(tools / 'bin/ninja')
        with patch.object(bootstrap, 'unpack'), \
             patch.object(bootstrap.subprocess, 'check_output', return_value='1.12.1') as version:
            with self.assertRaisesRegex(RuntimeError, 'Ninja does not match pinned'):
                bootstrap.install_build_tools(tools, 'macos', VERSIONS, MagicMock())
        self.assertEqual(version.call_count, 2)
        self.assertTrue(ninja.exists())


if __name__ == '__main__':
    unittest.main()
