import importlib.util
import os
from pathlib import Path
import platform
import tempfile
import unittest
from unittest.mock import patch, MagicMock
from contextlib import redirect_stdout
from io import StringIO
import subprocess
from types import SimpleNamespace
import json

ROOT = Path(__file__).resolve().parents[1]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / (name + '.py'))
    value = importlib.util.module_from_spec(spec)
    if name in ('test-rdp', 'test-cocoa'):
        with patch.dict('sys.modules', {'docker': module('docker'), 'env': module('env')}):
            spec.loader.exec_module(value)
    else:
        spec.loader.exec_module(value)
    return value


class ProjectTools(unittest.TestCase):
    def test_project_build_target_rejects_options_and_nonbuild_commands(self):
        with patch.dict('sys.modules', {'env': module('env')}): project = module('project')
        for arguments in (['build', '--target=--clean-first'], ['test', '--target', 'qt_smoke'], ['build', '--target', '../outside']):
            with patch('sys.argv', ['project.py', *arguments]), patch.object(project, 'environment') as environment, \
                    patch.object(project.subprocess, 'run') as run, redirect_stdout(StringIO()):
                with self.assertRaises(SystemExit): project.main()
                environment.assert_not_called(); run.assert_not_called()

    def test_session_features_runner_targets_and_oracles(self):
        rdp = module('test-rdp')
        self.assertIn('rdp_session_integration', module('test-cocoa').TARGETS)
        with patch.dict('sys.modules', {'env': module('env')}):
            self.assertIn('rdp_session_integration', module('test-native').TARGETS)
        self.assertIn('/tmp/rm-cursors', rdp.session_features_check('remoteCursorsThroughShell'))
        clipboard = rdp.session_features_check('bidirectionalTextClipboardThroughShell')
        self.assertIn('/tmp/rm-session-paste', clipboard)
        for key in ('F6', 'F7', 'F8'): self.assertIn(key, clipboard)
        with self.assertRaises(ValueError): rdp.session_features_check('unownedCase')

    def test_xml_live_fixture_inputs_require_independent_pairs(self):
        rdp = module('test-rdp')
        first = dict(port=41001, password='synthetic-server-one', master='synthetic-master-one')
        second = dict(port=41002, password='synthetic-server-two', master='synthetic-master-two')
        values = rdp.fixture_inputs('multipleConnectionFilesThroughShell', first, second)
        self.assertTrue(values['RM_RDP_TEST_PASSWORD_2'] == second['password'])
        self.assertTrue(values['RM_XML_TEST_MASTER_2'] == second['master'])
        self.assertEqual(values['RM_RDP_TEST_PORT_2'], '41002')
        with self.assertRaises(ValueError):
            rdp.fixture_inputs('multipleConnectionFilesThroughShell', first)
        for field in ('port', 'password', 'master'):
            alias = dict(second, **{field: first[field]})
            with self.assertRaises(ValueError):
                rdp.fixture_inputs('multipleConnectionFilesThroughShell', first, alias)

    def test_xml_live_server_oracles_distinguish_restart_and_owner(self):
        rdp = module('test-rdp')
        required, forbidden = rdp.xml_input_keys('configPasswordXmlThroughShell')
        self.assertEqual(required, {'a', 'b'})
        self.assertFalse(forbidden)
        required, forbidden = rdp.xml_input_keys('multipleConnectionFilesThroughShell')
        self.assertEqual(required, {'a', 'c', 'e', 'g'})
        self.assertEqual(forbidden, {'b', 'd', 'f'})
        required, forbidden = rdp.xml_input_keys('multipleConnectionFilesThroughShell', secondary=True)
        self.assertEqual(required, {'b', 'd', 'f'})
        self.assertEqual(forbidden, {'a', 'c', 'e', 'g'})

    def test_xml_live_server_oracle_rejects_missing_or_crossed_input(self):
        rdp = module('test-rdp')
        for case, secondary in [('configPasswordXmlThroughShell', False),
                                ('multipleConnectionFilesThroughShell', False), ('multipleConnectionFilesThroughShell', True)]:
            required, forbidden = rdp.xml_input_keys(case, secondary)
            valid = ''.join('2 ' + key + ' 0 0\n' for key in sorted(required)) + '4 ?? 400 300\n4 ?? 200 150\n'
            samples = [valid, valid.replace('2 ' + sorted(required)[0] + ' 0 0\n', ''), valid.replace('4 ??', '5 ??')]
            if forbidden: samples.append(valid + '2 ' + sorted(forbidden)[0] + ' 0 0\n')
            for index, text in enumerate(samples):
                with patch('pathlib.Path.read_text', return_value=text), redirect_stdout(StringIO()):
                    if index == 0: exec(rdp.xml_input_check(case, secondary), {})
                    else:
                        with self.assertRaises(AssertionError): exec(rdp.xml_input_check(case, secondary), {})

    def test_xml_live_two_fixtures_are_scoped_and_cleanup_is_reverse_owned(self):
        runner = module('test-rdp')
        for failure in ('none', 'secondary-start', 'client'):
            with self.workspace() as directory:
                calls = []
                def run(command, **kwargs):
                    calls.append((command, dict(kwargs.get('env', {}))))
                    if failure == 'secondary-start' and 'run' in command and '--network' in command:
                        raise subprocess.CalledProcessError(1, command)
                    return SimpleNamespace(returncode=int(failure == 'client' and str(command[0]).endswith('rdp_integration')), stdout='safe', stderr='')
                def output(command, **kwargs):
                    if '-functions' in command: return 'multipleConnectionFilesThroughShell()\n'
                    if 'inspect' in command: return json.dumps([{'NetworkSettings': {'Ports': {
                        '3389/tcp': [{'HostPort': '41001'}], '3390/tcp': [{'HostPort': '41002'}]}}}])
                    return 'fixture-packages'
                with patch('sys.argv', ['test-rdp.py', 'multipleConnectionFilesThroughShell']), \
                     patch.object(runner, 'configuration', return_value=(['docker'], {}, 'unit', 'arm64')), \
                     patch.object(runner, 'environment', return_value={'RM_BUILD_DIR': directory}), \
                     patch.object(runner.secrets, 'token_urlsafe', side_effect=['synthetic-server-one', 'synthetic-master-one', 'synthetic-server-two', 'synthetic-master-two']), \
                     patch.object(runner.subprocess, 'run', side_effect=run), patch.object(runner.subprocess, 'check_output', side_effect=output), \
                     patch.object(runner.socket, 'create_connection', return_value=MagicMock()), redirect_stdout(StringIO()) as captured:
                    if failure == 'none': runner.main()
                    else:
                        with self.assertRaises((RuntimeError, subprocess.CalledProcessError)): runner.main()
                primary = 'remotemanager-rdp-test-unit-arm64'; secondary = primary + '-secondary'
                removed = [command[-1] for command, _ in calls if 'rm' in command]
                self.assertEqual(removed, [primary] if failure == 'secondary-start' else [secondary, primary])
                starts = [(command, env) for command, env in calls if 'run' in command and '-d' in command]
                self.assertEqual(len(starts), 2)
                self.assertIn('127.0.0.1::3389', starts[0][0]); self.assertIn('127.0.0.1::3390', starts[0][0])
                self.assertIn('container:' + primary, starts[1][0]); self.assertNotIn('-p', starts[1][0])
                self.assertTrue(starts[0][1]['RM_RDP_TEST_PASSWORD'] != starts[1][1]['RM_RDP_TEST_PASSWORD'])
                for command, _ in calls:
                    self.assertNotIn('prune', command)
                    for secret in ('synthetic-server-one', 'synthetic-master-one', 'synthetic-server-two', 'synthetic-master-two'):
                        self.assertNotIn(secret, command); self.assertNotIn(secret, captured.getvalue())
                if failure == 'none':
                    checks = [command for command, _ in calls if 'exec' in command and '/tmp/rm-input' in command[-1]]
                    self.assertEqual([command[2] for command in checks], [primary, secondary])

    def test_xml_live_native_client_forwards_pair_without_argument_secrets(self):
        with patch.dict('sys.modules', {'env': module('env')}): runner = module('test-native')
        values = {'RM_RDP_TEST_PORT': '3389', 'RM_RDP_TEST_PORT_2': '3390',
                  'RM_RDP_TEST_PASSWORD': 'synthetic-server-one', 'RM_XML_TEST_MASTER': 'synthetic-master-one',
                  'RM_RDP_TEST_PASSWORD_2': 'synthetic-server-two', 'RM_XML_TEST_MASTER_2': 'synthetic-master-two'}
        args = ('rdp_integration', ['multipleConnectionFilesThroughShell'], 'xcb', ['docker'],
                {'LOCAL_UID': '501', 'LOCAL_GID': '20'}, 'aabbccddeeff', 'arm64', 'remotemanager-rdp-test-aabbccddeeff-arm64', values)
        with patch.object(runner.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, 'safe', '')) as runs:
            runner.container_client(*args)
        created = runs.call_args_list[0]
        for key, value in values.items():
            self.assertEqual(created.kwargs['env'][key], value)
            self.assertIn(key, created.args[0])
            if 'PORT' not in key: self.assertNotIn(value, created.args[0])
        for secret in (values['RM_RDP_TEST_PASSWORD_2'], values['RM_XML_TEST_MASTER_2']):
            with patch.object(runner.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, secret, '')):
                with self.assertRaisesRegex(RuntimeError, 'suppressed'): runner.container_client(*args)

    def test_xml_live_cocoa_launcher_forwards_secondary_inputs(self):
        runner = module('test-cocoa')
        with self.workspace() as directory:
            build = Path(directory); (build / 'app').mkdir(); (build / 'app/rdp_integration').write_text('owned placeholder')
            values = {'RM_RDP_TEST_PASSWORD_2': 'synthetic-server-two', 'RM_XML_TEST_MASTER_2': 'synthetic-master-two', 'RM_RDP_TEST_PORT_2': '3390'}
            def launch(command, **kwargs):
                for key, value in values.items(): self.assertIn(key + '=' + value, command)
                fifo = Path(command[command.index('--stdout') + 1])
                with fifo.open('wb', buffering=0) as stream:
                    stream.write(b'Totals: 3 passed, 0 failed, 0 skipped, 10ms\n********* Finished testing of RdpIntegration *********\n')
                return SimpleNamespace(stdout=b'', stderr=b'')
            with patch.object(runner.sys, 'platform', 'darwin'), patch.object(runner.subprocess, 'run', side_effect=launch):
                result = runner.run_cocoa('rdp_integration', ['multipleConnectionFilesThroughShell'], {'RM_BUILD_DIR': directory, **values}, tuple(values.values())[:2])
            self.assertEqual(result.returncode, 0)

    def test_xml_live_secondary_secrets_are_redacted(self):
        rdp = module('test-rdp')
        secrets = ('synthetic-server-one', 'synthetic-master-one', 'synthetic-server-two', 'synthetic-master-two')
        for secret in secrets:
            with self.assertRaises(RuntimeError) as error:
                rdp.checked_output('diagnostic ' + secret, *secrets)
            self.assertTrue(all(value not in str(error.exception) for value in secrets))

    def workspace(self):
        base = ROOT / 'build' / ('macos' if platform.system() == 'Darwin' else 'linux') / 'test-data'
        base.mkdir(parents=True, exist_ok=True)
        return tempfile.TemporaryDirectory(dir=base)

    def qt_files(self, qt, shadertools=True):
        paths = ['lib/cmake/Qt6/Qt6Config.cmake']
        if shadertools:
            paths += ['lib/cmake/Qt6ShaderTools/Qt6ShaderToolsConfig.cmake',
                      'lib/cmake/Qt6ShaderToolsTools/Qt6ShaderToolsToolsConfig.cmake',
                      'lib/cmake/Qt6ShaderToolsTools/Qt6ShaderToolsMacros.cmake', 'bin/qsb']
        for name in paths:
            path = qt / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text('fixture')
        if shadertools: (qt / 'bin/qsb').chmod(0o755)

    def test_packages_credit_cisco_and_never_ship_openh264(self):
        with patch.dict('sys.modules', {'env': module('env')}): package = module('package')
        with self.workspace() as directory:
            for host, relative in [('macos', 'Contents/Resources/third-party'), ('linux', 'share/third-party')]:
                destination = Path(directory) / host
                package.stage_decoder_notice(Path(directory), destination, host)
                notice = (destination / relative / 'OpenH264-NOTICE.txt').read_text()
                self.assertIn('OpenH264 Video Codec provided by Cisco Systems, Inc.', notice)
                self.assertIn('does not include OpenH264', notice)
                package.reject_bundled_openh264(destination)
                library = destination / 'lib/libopenh264.so.8'; library.parent.mkdir(parents=True); library.write_bytes(b'x')
                with self.assertRaisesRegex(RuntimeError, 'must not contain OpenH264'): package.reject_bundled_openh264(destination)
        cisco = (ROOT / 'vendor/licenses/OpenH264-Cisco-BINARY_LICENSE.txt').read_text()
        self.assertIn('"OpenH264 Video Codec provided by Cisco Systems, Inc."', cisco)

    def linux_package_fixture(self, root):
        build = root / 'build/linux/amd64/app'; build.mkdir(parents=True)
        (build / 'RemoteManager').write_bytes(b'owned executable')
        qt = root / '.dev/linux/qt'; native = root / '.dev/linux/prefix'
        for name in ('platforms/libqxcb.so', 'xcbglintegrations/libqxcb-glx-integration.so',
                     'xcbglintegrations/libqxcb-egl-integration.so'):
            path = qt / 'plugins' / name; path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())
        for name in ('libcrypto.so.3', 'ossl-modules/legacy.so'):
            path = native / 'lib' / name; path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())
        return dict(os.environ, RM_PLATFORM='linux', RM_BUILD_DIR=str(build.parent), RM_APP_BUILD_DIR=str(build),
                    RM_QT_PREFIX=str(qt), RM_NATIVE_PREFIX=str(native))

    def run_linux_package_fixture(self, root, env):
        with patch.dict('sys.modules', {'env': module('env')}): package = module('package')
        def run(command, **kwargs):
            output = ''
            if command[0] == 'ldd': output = 'libcrypto.so.3 => ' + env['RM_NATIVE_PREFIX'] + '/lib/libcrypto.so.3 (0x123)\n'
            return subprocess.CompletedProcess(command, 0, output, '')
        with patch.object(package, 'ROOT', root), patch.object(package, 'environment', return_value=env), \
             patch.object(package, 'stage_decoder_notice'), patch.object(package, 'stage_ssh_notices'), \
             patch.object(package, 'stage_ui_notices'), patch.object(package, 'stage_branding'), \
             patch.object(package.subprocess, 'run', side_effect=run) as calls, \
             patch('sys.argv', ['package.py']), redirect_stdout(StringIO()):
            package.main()
        return root / 'dist/linux/amd64/RemoteManager', calls.call_args_list

    def test_linux_package_includes_xcb_gl_integrations(self):
        with self.workspace() as directory:
            root = Path(directory); env = self.linux_package_fixture(root)
            artifact, calls = self.run_linux_package_fixture(root, env)
            for name in ('libqxcb-glx-integration.so', 'libqxcb-egl-integration.so'):
                path = artifact / 'plugins/xcbglintegrations' / name
                self.assertEqual(path.read_bytes(), (Path(env['RM_QT_PREFIX']) / 'plugins/xcbglintegrations' / name).read_bytes())
                self.assertTrue(any(call.args[0][0] == 'ldd' and call.args[0][1] == str(path) for call in calls))
                self.assertTrue(any(call.args[0][0] == 'patchelf' and call.args[0][-1] == str(path) for call in calls))

    def test_linux_package_includes_matching_legacy_provider(self):
        with self.workspace() as directory:
            root = Path(directory); env = self.linux_package_fixture(root)
            artifact, calls = self.run_linux_package_fixture(root, env)
            provider = artifact / 'lib/ossl-modules/legacy.so'
            self.assertEqual(provider.read_bytes(), (Path(env['RM_NATIVE_PREFIX']) / 'lib/ossl-modules/legacy.so').read_bytes())
            self.assertEqual((artifact / 'lib/libcrypto.so.3').read_bytes(), b'libcrypto.so.3')
            self.assertTrue(any(call.args[0][0] == 'ldd' and call.args[0][1] == str(provider) for call in calls))
            self.assertTrue(any(call.args[0][:3] == ['patchelf', '--set-rpath', '$ORIGIN/..'] and call.args[0][-1] == str(provider) for call in calls))

    def test_linux_package_rejects_missing_required_runtime(self):
        missing = ('qt/plugins/xcbglintegrations/libqxcb-glx-integration.so',
                   'qt/plugins/xcbglintegrations/libqxcb-egl-integration.so',
                   'prefix/lib/ossl-modules/legacy.so')
        for name in missing:
            with self.subTest(name=name), self.workspace() as directory:
                root = Path(directory); env = self.linux_package_fixture(root)
                (root / '.dev/linux' / name).unlink()
                with self.assertRaisesRegex(SystemExit, 'Missing required Linux runtime'):
                    self.run_linux_package_fixture(root, env)
                self.assertFalse((root / 'dist/linux/amd64/RemoteManager').exists())
                self.assertFalse((root / 'dist/linux/amd64/RemoteManager-linux-amd64.tar.gz').exists())

    def test_linux_package_runtime_paths_are_contained(self):
        before = dict(os.environ)
        with self.workspace() as directory:
            root = Path(directory); env = self.linux_package_fixture(root)
            artifact, calls = self.run_linux_package_fixture(root, env)
            launcher = (artifact / 'RemoteManager').read_text()
            self.assertIn('export OPENSSL_MODULES="$DIR/lib/ossl-modules"', launcher)
            self.assertIn('export QT_PLUGIN_PATH="$DIR/plugins"', launcher)
            self.assertNotIn(str(root), launcher)
            for call in calls:
                command = call.args[0]
                if command[0] == 'patchelf':
                    self.assertTrue(command[2].startswith('$ORIGIN/'))
                    path = Path(command[-1])
                    resolved = (path.parent / command[2].removeprefix('$ORIGIN/')).resolve()
                    self.assertEqual(resolved, artifact / 'lib')
            original = (artifact / 'bin/RemoteManager').read_bytes()
            with self.assertRaisesRegex(SystemExit, 'Archive already exists'):
                self.run_linux_package_fixture(root, env)
            self.assertEqual((artifact / 'bin/RemoteManager').read_bytes(), original)
        self.assertEqual(dict(os.environ), before)

    def test_package_audits_dependencies_and_runtime_search_paths(self):
        with patch.dict('sys.modules', {'env': module('env')}): package = module('package')
        listing = ('owned (architecture arm64):\n'
                   '\t@rpath/libopenh264.2.6.0.dylib (compatibility version 0.0.0)\n'
                   '\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n')
        package.audit_macho_loads(listing)
        for path in ('/unowned/lib.dylib', str(ROOT / '.dev/macos/prefix/lib/libopenh264.dylib')):
            with self.assertRaises(RuntimeError): package.audit_macho_loads(listing + '\t' + path + ' (compatibility version 0.0.0)\n')
        runtime = 'cmd LC_RPATH\n cmdsize 72\n path @executable_path/../Frameworks (offset 12)\n'
        self.assertEqual(package.macho_runtime_paths(runtime), ['@executable_path/../Frameworks'])
        with self.workspace() as directory:
            bundle = Path(directory) / 'Owned.app'; loader = bundle / 'Contents/Frameworks/lib.dylib'
            executable = bundle / 'Contents/MacOS/RemoteManager'
            for path in ('@executable_path/../Frameworks', '@loader_path/'):
                package.audit_macho_runtime(path, bundle, loader, executable)
            for path in ('/unowned', str(ROOT / '.dev/macos/qt/lib'), '@loader_path/../../../outside'):
                with self.assertRaises(RuntimeError): package.audit_macho_runtime(path, bundle, loader, executable)

    def test_bootstrap_existing_qt_installs_missing_shadertools(self):
        bootstrap = module('bootstrap')
        with self.workspace() as directory:
            qt = Path(directory); self.qt_files(qt, False)
            fetch = MagicMock(return_value=Path('owned-shader-archive'))
            with patch.object(bootstrap, 'unpack') as unpack:
                bootstrap.install_qt_modules(qt, 'macos', {'qtbase-macos': {}, 'qtshadertools-macos': {}}, fetch)
            fetch.assert_called_once_with('qtshadertools-macos'); unpack.assert_called_once_with(Path('owned-shader-archive'), qt)
            self.qt_files(qt); (qt / 'bin/qsb').chmod(0o644)
            self.assertFalse(bootstrap.qt_module_installed(qt, 'qtshadertools'))

    def test_bootstrap_complete_qt_skips_installed_modules(self):
        bootstrap = module('bootstrap')
        with self.workspace() as directory:
            qt = Path(directory); self.qt_files(qt)
            fetch = MagicMock()
            with patch.object(bootstrap, 'unpack') as unpack:
                bootstrap.install_qt_modules(qt, 'macos', {'qtbase-macos': {}, 'qtshadertools-macos': {}}, fetch)
            fetch.assert_not_called(); unpack.assert_not_called()

    def test_linux_image_supplies_qt_gui_private_xkb_headers(self):
        import shlex
        source = (ROOT / 'Dockerfile').read_text().split('AS rdp-fixture')[0]
        self.assertIn('libxkbcommon-dev', shlex.split(source.replace('\\\n', ' ')))

    def test_linux_comparator_has_native_window_development_headers(self):
        import shlex
        source = (ROOT / 'Dockerfile').read_text().split('AS rdp-fixture')[0]
        packages = shlex.split(source.replace('\\\n', ' '))
        for name in ('libx11-dev', 'libxext-dev', 'libwayland-dev'):
            self.assertIn(name, packages)

    def test_linux_qsb_runtime_prerequisites_precede_bootstrap(self):
        source = (ROOT / 'Dockerfile').read_text().split('RUN python3 -m venv')[0]
        self.assertIn('libglib2.0-0t64', source.split('RUN apt-get update')[1])

    def test_linux_image_includes_bootstrap_shader_inputs(self):
        import shlex
        import shutil
        lines = (ROOT / 'Dockerfile').read_text().splitlines()
        with self.workspace() as directory:
            stage = Path(directory)
            for line in lines:
                if line.startswith('RUN python3 -m venv'): break
                if not line.startswith('COPY '): continue
                sources, destination = shlex.split(line)[1:-1], shlex.split(line)[-1]
                destination = stage / destination.removeprefix('./')
                destination.mkdir(parents=True, exist_ok=True)
                for source in sources:
                    path = ROOT / source
                    if path.is_dir(): shutil.copytree(path, destination, dirs_exist_ok=True)
                    else: shutil.copy2(path, destination / path.name)
            for name in ('desktop.vert', 'desktop.frag'):
                self.assertEqual((stage / 'app/shaders' / name).read_bytes(), (ROOT / 'app/shaders' / name).read_bytes())

    def test_bootstrap_qsb_scopes_bundled_icu_lookup_to_child(self):
        bootstrap = module('bootstrap'); before = dict(os.environ)
        with self.workspace() as directory:
            qt = Path(directory); self.qt_files(qt)
            def version(command, **kwargs):
                paths = kwargs['env']['LD_LIBRARY_PATH'].split(os.pathsep)
                self.assertEqual(paths[:2], [str(qt / 'lib'), str(qt)])
                return 'qsb 6.12.0'
            def compile_shader(*command, **kwargs):
                self.assertIn(str(qt), kwargs['env']['LD_LIBRARY_PATH'].split(os.pathsep))
                Path(command[command.index('-o') + 1]).write_bytes(b'owned shader package')
            with patch.object(bootstrap.subprocess, 'check_output', side_effect=version), \
                 patch.object(bootstrap, 'run', side_effect=compile_shader), patch.object(bootstrap.subprocess, 'run') as dump:
                bootstrap.validate_shadertools(qt, qt / 'checks')
                self.assertEqual(dump.call_count, 2)
                for call in dump.call_args_list: self.assertIn(str(qt), call.kwargs['env']['LD_LIBRARY_PATH'].split(os.pathsep))
        self.assertEqual(dict(os.environ), before)

    def test_bootstrap_qsb_failure_keeps_loader_diagnostic(self):
        bootstrap = module('bootstrap')
        with self.workspace() as directory:
            qt = Path(directory); self.qt_files(qt)
            error = subprocess.CalledProcessError(127, ['qsb', '--version'], output='owned loader error')
            with patch.object(bootstrap.subprocess, 'check_output', side_effect=error):
                with self.assertRaisesRegex(RuntimeError, 'owned loader error'):
                    bootstrap.validate_shadertools(qt, qt / 'checks')

    def test_bootstrap_shadertools_must_compile_usable_packages(self):
        bootstrap = module('bootstrap')
        with self.workspace() as directory:
            qt = Path(directory); self.qt_files(qt)
            with patch.object(bootstrap.subprocess, 'check_output', return_value='qsb 6.11.2'):
                with self.assertRaisesRegex(RuntimeError, 'pinned Qt'): bootstrap.validate_shadertools(qt, qt / 'checks')
            with patch.object(bootstrap.subprocess, 'check_output', return_value='qsb 6.12.0'), patch.object(bootstrap, 'run'):
                with self.assertRaisesRegex(RuntimeError, 'no shader package'): bootstrap.validate_shadertools(qt, qt / 'checks')

    def test_command_environment_preserves_home_and_caller(self):
        before = dict(os.environ)
        env = module('env').environment()
        self.assertEqual(dict(os.environ), before)
        self.assertEqual(env.get('HOME'), before.get('HOME'))
        for name in ('CARGO_HOME', 'CARGO_TARGET_DIR', 'XDG_CACHE_HOME', 'PIP_CACHE_DIR'):
            self.assertTrue(Path(env[name]).is_relative_to(ROOT))
        expected = 'macos' if platform.system() == 'Darwin' else 'linux'
        self.assertIn('/' + expected + '/', env['CARGO_TARGET_DIR'])

    def test_cleanup_preserves_source_data_and_other_platform(self):
        cleanup = module('cleanup')
        base = ROOT / 'build' / ('macos' if platform.system() == 'Darwin' else 'linux') / 'test-data'
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as directory:
            root = Path(directory)
            for relative in ('Cargo.toml', 'src/main.cpp', '.dev/macos/data/profiles.json', '.dev/macos/data/confCons.xml', '.dev/linux/qt/keep', 'build/macos/old', 'dist/macos/old', '.dev/macos/qt/old'):
                p = root / relative; p.parent.mkdir(parents=True, exist_ok=True); p.write_text('keep')
            cleanup.remove(cleanup.targets(root, 'macos', 'all'))
            for relative in ('Cargo.toml', 'src/main.cpp', '.dev/macos/data/profiles.json', '.dev/macos/data/confCons.xml', '.dev/linux/qt/keep'):
                self.assertTrue((root / relative).exists())
            self.assertFalse((root / 'build/macos').exists())
            cleanup.remove(cleanup.targets(root, 'macos', 'data'))
            self.assertFalse((root / '.dev/macos/data').exists())

    def test_cleanup_rejects_symlink_escape(self):
        cleanup = module('cleanup')
        base = ROOT / 'build' / ('macos' if platform.system() == 'Darwin' else 'linux') / 'test-data'
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as directory:
            root = Path(directory) / 'project'; root.mkdir()
            outside = Path(directory) / 'unrelated'; outside.mkdir()
            (root / 'build').symlink_to(outside, target_is_directory=True)
            with self.assertRaises(ValueError): cleanup.targets(root, 'macos', 'build')
            self.assertTrue(outside.exists())

    def test_docker_cleanup_preserves_unrelated_image_tags(self):
        tool = module('docker')
        _, _, instance, _ = tool.configuration()
        tag = 'remotemanager-dev:' + instance + '-arm64'
        calls = []
        def fake_run(command, **kwargs):
            calls.append(command)
            output = ''
            code = 0
            if 'ps' in command: output = 'owned-container\n'
            if 'image' in command and 'ls' in command: output = 'owned-image\n'
            if 'image' in command and 'inspect' in command:
                output = json.dumps([{'Config': {'Labels': {'org.remotemanager.project': 'remotemanager', 'org.remotemanager.instance': instance}}, 'RepoTags': [tag, 'unrelated:keep']}])
            if 'buildx' in command and 'inspect' in command: code = 1
            return SimpleNamespace(returncode=code, stdout=output)
        with patch('sys.argv', ['docker.py', 'clean', '--yes']), patch.object(tool.subprocess, 'run', side_effect=fake_run):
            tool.main()
        image_removals = [c for c in calls if 'image' in c and 'rm' in c]
        self.assertEqual(len(image_removals), 1)
        self.assertEqual(image_removals[0][-1], tag)
        for c in calls:
            self.assertNotIn('unrelated:keep', c)
            self.assertNotIn('prune', c)
            if 'ps' in c or ('image' in c and 'ls' in c):
                self.assertIn('label=org.remotemanager.instance=' + instance, c)
    def test_cocoa_runner_includes_multiple_files(self):
        self.assertIn('multiple_files_test', module('test-cocoa').TARGETS)

    def test_cocoa_runner_includes_password_editing(self):
        self.assertIn('password_editing_test', module('test-cocoa').TARGETS)

    def test_native_runners_include_terminal_shortcut_tests(self):
        for name in ('test-cocoa', 'test-native'):
            with self.subTest(runner=name):
                targets = module(name).TARGETS
                self.assertIn('terminal_test', targets)
                self.assertIn('terminal_appearance_test', targets)

    def test_native_runners_include_xml_storage_without_legacy_workflows(self):
        for name in ('test-cocoa', 'test-native'):
            with self.subTest(runner=name):
                targets = module(name).TARGETS
                self.assertIn('xml_storage_test', targets)
                self.assertNotIn('legacy_conversion_test', targets)
                self.assertNotIn('vault_shell_test', targets)

    def test_cocoa_runner_requires_complete_successful_report(self):
        runner = module('test-cocoa')
        totals = 'Totals: 3 passed, 0 failed, 0 skipped, 0 blacklisted, 123ms\n'
        footer = '********* Finished testing of Smoke *********\n'
        self.assertTrue(runner.successful_report(totals + footer))
        for report in ('', footer, totals, totals.replace('0 failed', '1 failed') + footer,
                       totals.replace('3 passed', '0 passed') + footer, totals + footer + 'QFATAL'):
            with self.subTest(report=report):
                self.assertFalse(runner.successful_report(report))

    def test_cocoa_runner_forwards_graphics_policy(self):
        runner = module('test-cocoa')
        with self.workspace() as directory:
            build = Path(directory); (build / 'app').mkdir(); (build / 'app/performance_client').write_text('owned placeholder')
            def launch(command, **kwargs):
                self.assertIn('RM_PERFORMANCE_POLICY=gfx', command)
                fifo = Path(command[command.index('--stdout') + 1])
                with fifo.open('wb', buffering=0) as stream:
                    stream.write(b'Totals: 3 passed, 0 failed, 0 skipped, 10ms\n********* Finished testing of PerformanceClient *********\n')
                return SimpleNamespace(stdout=b'', stderr=b'')
            with patch.object(runner.sys, 'platform', 'darwin'), patch.object(runner.subprocess, 'run', side_effect=launch):
                result = runner.run_cocoa('performance_client', [], {'RM_BUILD_DIR': directory, 'RM_PERFORMANCE_POLICY': 'gfx'})
            self.assertEqual(result.returncode, 0)

    def test_cocoa_runner_checks_secrets_before_saving_reports(self):
        runner = module('test-cocoa')
        for contents in ('Totals: 3 passed, 0 failed, 0 skipped, 10ms\n********* Finished testing of Smoke *********\n', 'fixture-secret'):
            with self.workspace() as directory:
                build = Path(directory); (build / 'app').mkdir(); (build / 'app/qt_smoke').write_text('owned placeholder')
                def launch(command, **kwargs):
                    fifo = Path(command[command.index('--stdout') + 1])
                    with fifo.open('wb', buffering=0) as stream: stream.write(contents.encode())
                    return SimpleNamespace(stdout=b'', stderr=b'')
                with patch.object(runner.sys, 'platform', 'darwin'), patch.object(runner.subprocess, 'run', side_effect=launch):
                    if contents == 'fixture-secret':
                        with self.assertRaisesRegex(RuntimeError, 'output suppressed'):
                            runner.run_cocoa('qt_smoke', [], {'RM_BUILD_DIR': directory}, ('fixture-secret',))
                        self.assertFalse(list(build.rglob('results.txt')))
                    else:
                        result = runner.run_cocoa('qt_smoke', [], {'RM_BUILD_DIR': directory}, ('fixture-secret',))
                        self.assertEqual(result.returncode, 0)
                        self.assertEqual(len(list(build.rglob('results.txt'))), 1)
                self.assertFalse(list(build.rglob('output.pipe')))

    def test_rdp_runner_cocoa_preserves_fixture_secrecy_and_cleanup(self):
        runner = module('test-rdp')
        with self.workspace() as directory:
            def output(command, **kwargs):
                if '-functions' in command: return 'savedConnectionThroughShell()\n'
                if 'inspect' in command: return json.dumps([{'NetworkSettings': {'Ports': {'3389/tcp': [{'HostPort': '3399'}]}}}])
                return 'fixture-package-inventory'
            with patch('sys.argv', ['test-rdp.py', '--platform', 'cocoa', 'savedConnectionThroughShell']), \
                 patch.object(runner.sys, 'platform', 'darwin'), patch.object(runner, 'configuration', return_value=(['docker'], {}, 'unit', 'arm64')), \
                 patch.object(runner, 'environment', return_value={'RM_BUILD_DIR': directory}), \
                 patch.object(runner.secrets, 'token_urlsafe', side_effect=['fixture-secret', 'master-secret']), \
                 patch.object(runner.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout='safe', stderr='')) as runs, \
                 patch.object(runner.subprocess, 'check_output', side_effect=output), \
                 patch.object(runner.socket, 'create_connection', return_value=MagicMock()), \
                 patch.object(runner, 'cocoa_client', return_value=SimpleNamespace(returncode=0, stdout='safe', stderr='')) as native, \
                 redirect_stdout(StringIO()) as captured:
                runner.main()
            native.assert_called_once(); case, env, password, master = native.call_args.args
            self.assertEqual(case, 'savedConnectionThroughShell'); self.assertEqual(env['QT_QPA_PLATFORM'], 'cocoa')
            self.assertEqual((password, master), ('fixture-secret', 'master-secret'))
            self.assertTrue(any('rm' in call.args[0] for call in runs.call_args_list))
            self.assertNotIn(password, captured.getvalue()); self.assertNotIn(master, captured.getvalue())

    def test_rdp_runner_uses_owned_native_linux_client(self):
        runner = module('test-rdp')
        with self.workspace() as directory:
            def output(command, **kwargs):
                if '-functions' in command:
                    self.assertEqual(kwargs['env']['QT_QPA_PLATFORM'], 'offscreen')
                    return 'savedConnectionThroughShell()\n'
                if 'inspect' in command: return json.dumps([{'NetworkSettings': {'Ports': {'3389/tcp': [{'HostPort': '3399'}]}}}])
                return 'fixture-package-inventory'
            with patch('sys.argv', ['test-rdp.py', '--platform', 'wayland', 'savedConnectionThroughShell']), \
                 patch.object(runner, 'ROOT', Path(directory)), \
                 patch.object(runner, 'configuration', return_value=(['docker'], {}, 'unit', 'arm64')), \
                 patch.object(runner, 'environment', return_value={'RM_BUILD_DIR': directory}), \
                 patch.object(runner.secrets, 'token_urlsafe', side_effect=['fixture-secret', 'master-secret']), \
                 patch.object(runner.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout='safe', stderr='')), \
                 patch.object(runner.subprocess, 'check_output', side_effect=output), \
                 patch.object(runner.socket, 'create_connection', return_value=MagicMock()), \
                 patch.object(runner, 'linux_client', create=True, return_value=SimpleNamespace(returncode=0, stdout='safe', stderr='')) as native, \
                 redirect_stdout(StringIO()):
                runner.main()
            native.assert_called_once()
            self.assertEqual(native.call_args.args[0:2], ('savedConnectionThroughShell', 'wayland'))
            self.assertEqual(native.call_args.args[-1], {'RM_RDP_TEST_PASSWORD': 'fixture-secret',
                'RM_XML_TEST_MASTER': 'master-secret', 'RM_RDP_TEST_PORT': '3389'})

    def test_rdp_runner_rejects_secret_output(self):
        runner = module('test-rdp')
        self.assertEqual(runner.checked_output('safe diagnostics', 'fixture-secret'), 'safe diagnostics')
        with self.assertRaisesRegex(RuntimeError, 'output suppressed') as error:
            runner.checked_output('stderr contains fixture-secret', 'fixture-secret')
        self.assertNotIn('fixture-secret', str(error.exception))
        with self.assertRaisesRegex(RuntimeError, 'output suppressed') as error:
            runner.checked_output('stderr contains master-secret', 'fixture-secret', 'master-secret')
        self.assertNotIn('master-secret', str(error.exception))

    def test_rdp_runner_isolates_cases_and_cleans_owned_containers(self):
        runner = module('test-rdp')
        base = ROOT / 'build' / ('macos' if platform.system() == 'Darwin' else 'linux') / 'test-data'
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as directory:
            calls = []
            def run(command, **kwargs):
                calls.append((command, dict(kwargs.get('env', {}))))
                return SimpleNamespace(returncode=0, stdout='safe diagnostics', stderr='')
            def output(command, **kwargs):
                if '-functions' in command: return 'savedConnectionThroughShell()\nquickConnectThroughShell()\nprotectedConnectionsThroughShell()\n'
                if 'inspect' in command: return json.dumps([{'NetworkSettings': {'Ports': {'3389/tcp': [{'HostPort': '3399'}]}}}])
                return 'fixture-package-inventory'
            with patch('sys.argv', ['test-rdp.py']), patch.object(runner, 'configuration', return_value=(['docker'], {}, 'unit', 'arm64')), \
                 patch.object(runner, 'environment', return_value={'RM_BUILD_DIR': directory}), \
                 patch.object(runner.secrets, 'token_urlsafe', side_effect=['secret-one', 'master-one', 'secret-two', 'master-two', 'secret-three', 'master-three']), \
                 patch.object(runner.subprocess, 'run', side_effect=run), patch.object(runner.subprocess, 'check_output', side_effect=output), \
                 patch.object(runner.socket, 'create_connection', return_value=MagicMock()), redirect_stdout(StringIO()) as captured:
                runner.main()
            starts = [(c, env) for c, env in calls if 'run' in c and '-d' in c]
            self.assertEqual(len(starts), 3)
            self.assertEqual([env['RM_RDP_TEST_PASSWORD'] for _, env in starts], ['secret-one', 'secret-two', 'secret-three'])
            self.assertEqual(len([c for c, _ in calls if 'rm' in c]), 3)
            checks = [c for c, _ in calls if 'exec' in c]
            self.assertEqual(len(checks), 3)
            self.assertIn("assert 'b' in s", checks[-1][-1])
            self.assertIn('abs(x-200)<=3 and abs(y-150)<=3', checks[-1][-1])
            for secret in ('secret-one', 'master-one', 'secret-two', 'master-two', 'secret-three', 'master-three'):
                self.assertNotIn(secret, captured.getvalue())
                self.assertNotIn(secret, (Path(directory) / 'rdp-integration.log').read_text())
            for c, _ in calls:
                self.assertNotIn('prune', c)
            clients = [c for c, _ in calls if str(c[0]).endswith('rdp_integration')]
            self.assertEqual([c[-1] for c in clients], ['savedConnectionThroughShell', 'quickConnectThroughShell', 'protectedConnectionsThroughShell'])
            client_envs = [env for c, env in calls if str(c[0]).endswith('rdp_integration')]
            self.assertEqual([env['RM_XML_TEST_MASTER'] for env in client_envs], ['master-one', 'master-two', 'master-three'])
            self.assertTrue(all('RM_XML_TEST_MASTER' not in env for _, env in starts))

    def test_rdp_runner_failed_start_preserves_existing_container(self):
        runner = module('test-rdp')
        base = ROOT / 'build' / ('macos' if platform.system() == 'Darwin' else 'linux') / 'test-data'
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as directory:
            calls = []
            def run(command, **kwargs):
                calls.append(command)
                if 'run' in command and '-d' in command: raise subprocess.CalledProcessError(1, command)
                return SimpleNamespace(returncode=0)
            with patch('sys.argv', ['test-rdp.py']), patch.object(runner, 'configuration', return_value=(['docker'], {}, 'unit', 'arm64')), \
                 patch.object(runner, 'environment', return_value={'RM_BUILD_DIR': directory}), \
                 patch.object(runner.subprocess, 'run', side_effect=run), patch.object(runner.subprocess, 'check_output', return_value='savedConnectionThroughShell()\n'):
                with self.assertRaises(subprocess.CalledProcessError): runner.main()
            self.assertFalse(any('rm' in command for command in calls))

    def test_docker_names_context_and_uid_are_scoped(self):
        before = dict(os.environ)
        docker, env, instance, arch = module('docker').configuration('arm64')
        self.assertEqual(dict(os.environ), before)
        self.assertEqual(len(instance), 12)
        self.assertEqual(env['LOCAL_UID'], str(os.getuid()))
        self.assertEqual(arch, 'arm64')
        if platform.system() == 'Darwin' and 'RM_DOCKER_CONTEXT' not in before:
            self.assertEqual(docker, ['docker', '--context', 'orbstack'])
        source = (ROOT / 'scripts/docker.py').read_text()
        self.assertNotIn('system prune', source)
        self.assertNotIn('context use', source)
        self.assertIn('org.remotemanager.instance=', source)
        self.assertIn("'buildx', 'rm', builder", source)


if __name__ == '__main__': unittest.main()
