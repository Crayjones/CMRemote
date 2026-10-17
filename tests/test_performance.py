import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class PerformanceContract(unittest.TestCase):
    def workspace(self):
        import sys
        import tempfile
        base = ROOT / 'build' / ('macos' if sys.platform == 'darwin' else 'linux') / 'test-data'
        base.mkdir(parents=True, exist_ok=True)
        return tempfile.TemporaryDirectory(dir=base)

    def test_scratch_follows_native_platform(self):
        import sys
        from unittest.mock import patch
        with patch.object(sys, 'platform', 'linux'), self.workspace() as directory:
            self.assertTrue(Path(directory).is_relative_to(ROOT / 'build/linux'))

    def test_workload_is_deterministic_and_bounded(self):
        model = load('fixture_performance', ROOT / 'tests/rdp-fixture/performance.py')
        for workload in range(6):
            for step in (0, 1, 30, model.FRAMES):
                scene = model.scene(workload, step, 1024, 768)
                self.assertEqual(scene, model.scene(workload, step, 1024, 768))
                self.assertLessEqual(len(scene['rectangles']), 64)
                for x, y, width, height, color in scene['rectangles']:
                    self.assertTrue(0 <= x < 1024 and 0 <= y < 768)
                    self.assertTrue(0 < width <= 1024 - x and 0 < height <= 768 - y)
                    self.assertTrue(0 <= color <= 0xffffff)
        self.assertEqual(model.scene(0, 0, 1024, 768), model.scene(0, model.FRAMES, 1024, 768))
        with self.assertRaises(ValueError): model.scene(8, 1, 1024, 768)
        with self.assertRaises(ValueError): model.scene(1, 1, 8193, 768)

    def test_performance_result_rejects_secret_text_and_unbounded_rows(self):
        runner = load('performance_runner', ROOT / 'scripts/test-performance.py')
        safe = {'schema': 1, 'workload': 1, 'width': 1024, 'height': 768, 'dpr': 2,
                'window_width': 1024, 'window_height': 768, 'pixel_width': 2048, 'pixel_height': 1536,
                'trace': {'schema': 1, 'generation': 1, 'dropped': 0, 'codecs': [3],
                          'stages': [[i] + [0] * 13 for i in range(8)]}, 'pixels': []}
        self.assertEqual(runner.result('RM_PERFORMANCE_RESULT=' + json.dumps(safe), ()), safe)
        gfx = dict(safe, trace=dict(safe['trace'], codecs=[9, 10, 11, 12]))
        self.assertEqual(runner.result('RM_PERFORMANCE_RESULT=' + json.dumps(gfx), ()), gfx)
        for codecs in ([13], [-1], [True]):
            invalid = dict(safe, trace=dict(safe['trace'], codecs=codecs))
            with self.assertRaises(RuntimeError): runner.result('RM_PERFORMANCE_RESULT=' + json.dumps(invalid), ())
        modern = dict(safe, trace=dict(safe['trace'], schema=2, submission_gaps=[3, 16, 60, 60, 60, 50000000, 1]))
        self.assertEqual(runner.result('RM_PERFORMANCE_RESULT=' + json.dumps(modern), ()), modern)
        for gaps in ([0] * 6, [3, 60, 16, 60, 60, 50000000, 1], [3, 16, 60, 60, 60, 50000000, 4]):
            invalid = dict(modern, trace=dict(modern['trace'], submission_gaps=gaps))
            with self.assertRaises(RuntimeError): runner.result('RM_PERFORMANCE_RESULT=' + json.dumps(invalid), ())
        for malformed in (dict(safe, schema=True), dict(safe, trace=[]),
                          dict(safe, trace=dict(safe['trace'], schema=True))):
            with self.assertRaises(RuntimeError): runner.result('RM_PERFORMANCE_RESULT=' + json.dumps(malformed), ())
        unsafe = dict(safe, endpoint='private-host')
        with self.assertRaises(RuntimeError): runner.result('RM_PERFORMANCE_RESULT=' + json.dumps(unsafe), ())
        with self.assertRaises(RuntimeError): runner.result('synthetic-secret\nRM_PERFORMANCE_RESULT=' + json.dumps(safe), ('synthetic-secret',))
        unsafe = dict(safe, pixels=[0] * 10000)
        with self.assertRaises(RuntimeError): runner.result('RM_PERFORMANCE_RESULT=' + json.dumps(unsafe), ())

    def test_linux_capture_runs_under_owned_native_display(self):
        import tempfile
        from types import SimpleNamespace
        from unittest.mock import patch, MagicMock
        runner = load('native_performance_runner', ROOT / 'scripts/test-performance.py')
        safe = {'schema': 1, 'workload': 1, 'width': 1024, 'height': 768, 'dpr': 1,
                'window_width': 1024, 'window_height': 768, 'pixel_width': 1024, 'pixel_height': 768,
                'trace': {'schema': 1, 'generation': 1, 'dropped': 0, 'codecs': [3],
                          'stages': [[i] + [0] * 13 for i in range(8)]}, 'pixels': []}
        def output(command, **kwargs):
            if 'inspect' in command: return json.dumps([{'NetworkSettings': {'Ports': {'3389/tcp': [{'HostPort': '3399'}]}}}])
            return json.dumps({'frames': 180, 'elapsed_ns': 1000, 'width': 1024, 'height': 768, 'pixels': []})
        args = SimpleNamespace(platform='wayland', workload='sparse', policy='legacy', replay_baseline=None)
        with self.workspace() as directory, \
             patch.object(runner.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout='', stderr='')), \
             patch.object(runner.subprocess, 'check_output', side_effect=output), \
             patch.object(runner.socket, 'create_connection', return_value=MagicMock()), \
             patch.object(runner, 'linux_client', create=True, return_value=SimpleNamespace(returncode=0,
                stdout='RM_PERFORMANCE_RESULT=' + json.dumps(safe), stderr='')) as native:
            report, frames, elapsed, password = runner.capture('app', args, ['docker'], {}, 'unit', 'arm64',
                'owned-image', [], Path(directory), SimpleNamespace(FRAMES=180, expected_pixels=lambda *args: []),
                None, lambda: {'RM_PLATFORM': 'macos'})
        native.assert_called_once(); self.assertEqual(native.call_args.args[0], 'wayland')
        self.assertEqual(native.call_args.args[-1]['RM_RDP_TEST_PASSWORD'], password)
        self.assertEqual((report['dpr'], frames, elapsed), (1, 180, 1000))

    def test_linux_sdl_capture_uses_fixture_loopback_and_stdin(self):
        from types import SimpleNamespace
        from unittest.mock import patch, MagicMock
        runner = load('linux_sdl_capture', ROOT / 'scripts/test-performance.py')
        safe = {'schema': 1, 'workload': 1, 'width': 1024, 'height': 768, 'dpr': 1,
                'window_width': 1024, 'window_height': 768, 'pixel_width': 1024, 'pixel_height': 768,
                'trace': {'schema': 1, 'generation': 1, 'dropped': 0, 'codecs': [3],
                          'stages': [[i] + [0] * 13 for i in range(8)]}, 'pixels': []}
        def output(command, **kwargs):
            if 'inspect' in command: return json.dumps([{'NetworkSettings': {'Ports': {'3389/tcp': [{'HostPort': '3399'}]}}}])
            if 'hashlib,ssl' in command[-1]: return 'a' * 64
            return json.dumps({'frames': 180, 'elapsed_ns': 1000, 'width': 1024, 'height': 768, 'pixels': []})
        args = SimpleNamespace(platform='xcb', workload='sparse', policy='legacy', replay_baseline=None)
        cocoa = SimpleNamespace(successful_comparator_report=MagicMock(return_value=True))
        with self.workspace() as directory, \
             patch.object(runner.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout='', stderr='')), \
             patch.object(runner.subprocess, 'check_output', side_effect=output), \
             patch.object(runner.socket, 'create_connection', return_value=MagicMock()), \
             patch.object(runner, 'linux_comparator', create=True, return_value=SimpleNamespace(returncode=131,
                stdout='RM_PERFORMANCE_RESULT=' + json.dumps(safe), stderr='')) as native:
            (Path(directory) / 'test-data').mkdir()
            report, frames, elapsed, password = runner.capture('sdl', args, ['docker'], {}, 'unit', 'arm64',
                'owned-image', [], Path(directory), SimpleNamespace(FRAMES=180, expected_pixels=lambda *args: []),
                cocoa, lambda: {'RM_PLATFORM': 'macos'})
        native.assert_called_once(); command = native.call_args.args[-2]
        self.assertIn('/v:127.0.0.1:3389', command); self.assertNotIn('/v:127.0.0.1:3399', command)
        self.assertEqual(native.call_args.kwargs['stdin'], password + '\n')
        self.assertNotIn(password, ' '.join(command)); self.assertNotIn('RM_RDP_TEST_PASSWORD', native.call_args.args[-1])
        cocoa.successful_comparator_report.assert_called_once()
        self.assertEqual((report['dpr'], frames, elapsed), (1, 180, 1000))

    def test_linux_comparison_preserves_app_evidence_when_sdl_fails(self):
        from types import SimpleNamespace
        from unittest.mock import patch, MagicMock
        runner = load('failed_linux_comparison', ROOT / 'scripts/test-performance.py')
        app = {'trace': {'codecs': [3], 'dropped': 0}}
        modules = {'docker': SimpleNamespace(configuration=lambda: (['docker'], {}, 'aabbccddeeff', 'arm64')),
                   'env': SimpleNamespace(environment=lambda: {'RM_PLATFORM': 'macos'})}
        with self.workspace() as directory, patch.object(runner, 'ROOT', Path(directory)), \
             patch.dict('sys.modules', modules), \
             patch('sys.argv', ['test-performance.py', '--platform', 'xcb', '--workload', 'sparse']), \
             patch.object(runner, 'load', return_value=SimpleNamespace()), \
             patch.object(runner, 'linux_freerdp_description', return_value='Release; OpenH264 loaded at runtime; software-only'), \
             patch.object(runner.subprocess, 'run', return_value=SimpleNamespace(returncode=0)) as run, \
             patch.object(runner, 'capture', side_effect=[(app, 180, 1000, 'synthetic-app-secret'), RuntimeError('owned SDL failed')]), \
             patch('builtins.print'):
            self.assertEqual(runner.main(), 2)
            reports = list((Path(directory) / 'build/linux/arm64/performance').glob('*.json'))
            self.assertEqual(len(reports), 1); report = json.loads(reports[0].read_text())
            self.assertEqual(report['app'], app); self.assertNotIn('sdl', report)
            self.assertIn('owned SDL capture failed; matched baseline unavailable', report['blockers'])
            self.assertIn('client measurement intervals are not aligned to workload markers', report['blockers'])
            self.assertNotIn('synthetic-app-secret', reports[0].read_text())
            bootstrap = next(call for call in run.call_args_list if 'scripts/bootstrap-comparator.py' in call.args[0])
            self.assertIn('compose', bootstrap.args[0]); self.assertEqual(bootstrap.args[0][-1], 'linux')

    def test_comparator_cli_keeps_credentials_on_stdin_and_pins_owned_certificate(self):
        runner = load('comparator_cli', ROOT / 'scripts/test-performance.py')
        args = runner.comparator_arguments(12345, 'a' * 64)
        self.assertIn('/from-stdin:force', args)
        self.assertIn('/cert:fingerprint:sha256:' + 'a' * 64, args)
        self.assertIn('/t:RemoteManager owned comparator', args)
        self.assertIn('/cache:bitmap:off', args)  # Match the production RdpOptions default.
        for option in ('-clipboard', '-dynamic-resolution', '-gfx', '-rfx', '-nsc'):
            self.assertIn(option, args)
        self.assertFalse(any(arg.startswith(('/p:', '/cert:ignore', '/cert:tofu')) for arg in args))
        for port, fingerprint in ((0, 'a' * 64), (65536, 'a' * 64), (12345, 'not-a-fingerprint')):
            with self.assertRaises(ValueError): runner.comparator_arguments(port, fingerprint)

    def test_comparator_bootstrap_uses_selected_sdk(self):
        from types import SimpleNamespace
        from unittest.mock import patch, MagicMock
        runner = load('selected_comparator_sdk', ROOT / 'scripts/bootstrap-comparator.py')
        with self.workspace() as directory:
            root = Path(directory); (root / 'dependencies.lock.json').write_bytes((ROOT / 'dependencies.lock.json').read_bytes())
            prefix, qt = root / 'sdk/prefix', root / 'sdk/qt'
            header = prefix / 'include/freerdp3/freerdp/freerdp.h'; header.parent.mkdir(parents=True); header.write_text('owned header')
            env = {'PATH': '/owned', 'RM_NATIVE_PREFIX': str(prefix), 'RM_QT_PREFIX': str(qt),
                   'RM_BUILD_DIR': str(root / 'build/linux/arm64')}
            def fetch(artifact, cache):
                self.assertTrue(cache.is_dir()); self.assertTrue(cache.is_relative_to(root / '.dev/linux'))
                return Path('owned archive')
            bootstrap = SimpleNamespace(run=MagicMock(), unpack=MagicMock(), audit=SimpleNamespace(fetch=MagicMock(side_effect=fetch)))
            modules = {'comparator_bootstrap': bootstrap, 'comparator_env': SimpleNamespace(environment=lambda: env),
                       'comparator_probe': SimpleNamespace(instrument=MagicMock())}
            with patch('sys.argv', ['bootstrap-comparator.py', '--platform', 'linux', '--jobs', '1']), \
                 patch.object(runner, 'ROOT', root), patch.object(runner.platform, 'system', return_value='Linux'), \
                 patch.object(runner, 'load', side_effect=lambda name, path: modules[name]), \
                 patch.object(runner.shutil, 'which', return_value='owned cmake'), patch.object(runner.shutil, 'copyfile'):
                runner.main()
            configs = [call.args for call in bootstrap.run.call_args_list if '-S' in call.args]
            freerdp = next(args for args in configs if '-DWITH_CLIENT_SDL=ON' in args)
            self.assertIn('-DOPENSSL_ROOT_DIR=' + str(prefix), freerdp)
            self.assertIn('-DQt6_DIR=' + str(qt / 'lib/cmake/Qt6'), freerdp)
            self.assertTrue(any(str(root / 'build/linux/arm64/comparator') in str(value) for value in freerdp))
            import os
            version = next(call for call in bootstrap.run.call_args_list if '/version' in call.args)
            self.assertEqual(version.kwargs['env']['LD_LIBRARY_PATH'].split(os.pathsep)[0], str(root / '.dev/linux/comparator/prefix/lib'))
            self.assertNotIn('LD_LIBRARY_PATH', env)

    def test_comparator_bootstrap_creates_contained_download_cache(self):
        self.test_comparator_bootstrap_uses_selected_sdk()

    def test_comparator_runtime_prefers_its_own_libraries(self):
        self.test_comparator_bootstrap_uses_selected_sdk()

    def test_current_capture_reports_installed_decoder_configuration(self):
        import tempfile
        runner = load('performance_build', ROOT / 'scripts/test-performance.py')
        with self.workspace() as directory:
            prefix = Path(directory)
            header = prefix / 'include/freerdp3/freerdp/config.h'; header.parent.mkdir(parents=True)
            header.write_text('#define WITH_OPENH264\n#define WITH_GFX_H264\n#define WITH_OPENH264_LOADING\n')
            self.assertEqual(runner.app_freerdp_description(prefix, False), 'Release; OpenH264 loaded at runtime; software-only')
            header.write_text('/* #undef WITH_OPENH264 */\n/* #undef WITH_GFX_H264 */\n')
            self.assertEqual(runner.app_freerdp_description(prefix, False), 'Release; OpenH264/FFmpeg disabled')
            self.assertEqual(runner.app_freerdp_description(prefix, True), 'Release; OpenH264/FFmpeg disabled; frozen runtime')
            header.write_text('#define WITH_OPENH264\n#define WITH_GFX_H264\n#define WITH_FFMPEG\n')
            with self.assertRaises(RuntimeError): runner.app_freerdp_description(prefix, False)
            # A directly linked decoder would ship OpenH264 inside the app.
            header.write_text('#define WITH_OPENH264\n#define WITH_GFX_H264\n')
            with self.assertRaises(RuntimeError): runner.app_freerdp_description(prefix, False)

    def test_comparator_shutdown_requires_proven_deliberate_cancel(self):
        import sys
        sys.path.insert(0, str(ROOT / 'scripts'))
        cocoa = load('comparator_shutdown', ROOT / 'scripts/test-cocoa.py')
        lines = ['RM_PERFORMANCE_RESULT={}', 'RM_COMPARATOR_STOP_REQUESTED=1',
                 'RM_COMPARATOR_ABORT_CLEAN=1', 'RM_COMPARATOR_CANCELLED=1',
                 'RM_COMPARATOR_JOINED=1', 'RM_NATIVE_EXIT=131']
        report = '\n'.join(lines)
        self.assertTrue(cocoa.successful_comparator_report(report))
        self.assertTrue(cocoa.successful_comparator_report(report, 131))
        self.assertFalse(cocoa.successful_comparator_report(report, 1))
        for index in range(len(lines)):
            self.assertFalse(cocoa.successful_comparator_report('\n'.join(lines[:index] + lines[index + 1:])))
            self.assertFalse(cocoa.successful_comparator_report(report + '\n' + lines[index]))
        reordered = lines.copy(); reordered[1], reordered[2] = reordered[2], reordered[1]
        self.assertFalse(cocoa.successful_comparator_report('\n'.join(reordered)))
        success = '\n'.join([lines[0], lines[1], lines[4], 'RM_NATIVE_EXIT=0'])
        self.assertTrue(cocoa.successful_comparator_report(success))
        self.assertFalse(cocoa.successful_comparator_report(report.replace('131', '145')))

    def test_comparator_probe_requires_exact_source_anchors(self):
        probe = load('comparator_probe', ROOT / 'scripts/comparator-probe.py')
        self.assertEqual(probe.replace('abc', 'b', 'X'), 'aXc')
        for text in ('abcabc', 'missing'):
            with self.assertRaises(ValueError): probe.replace(text, 'abc', 'X')

    def test_baseline_audit_parses_each_architecture_without_accepting_external_libraries(self):
        baseline = load('performance_baseline', ROOT / 'scripts/performance-baseline.py')
        listing = ('/owned/baseline (architecture x86_64):\n'
                   '\t@rpath/QtCore.framework/Versions/A/QtCore (compatibility version 6.0.0)\n'
                   '/owned/baseline (architecture arm64):\n'
                   '\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n')
        baseline.audit_loads(listing)
        with self.assertRaises(RuntimeError): baseline.audit_loads(listing + '\t/unowned/lib.dylib (compatibility version 1.0.0)\n')

    def test_baseline_rejects_changed_dependencies_and_escaping_symlinks(self):
        import tempfile
        baseline = load('performance_baseline', ROOT / 'scripts/performance-baseline.py')
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            directory = build / 'performance/baseline-owned'
            bundle = directory / 'PerformanceBaseline.app'
            binary = bundle / 'Contents/MacOS/performance_client'
            binary.parent.mkdir(parents=True); binary.write_bytes(b'owned synthetic executable')
            manifest = {'schema': 1, 'policy': 'legacy-unoptimized', 'contained_runtime': True,
                        'files': baseline.files(bundle)}
            (directory / 'manifest.json').write_text(json.dumps(manifest))
            self.assertEqual(baseline.verify(directory, build), bundle.resolve())
            binary.write_bytes(b'changed executable')
            with self.assertRaises(RuntimeError): baseline.verify(directory, build)
            binary.with_name('escape').symlink_to(build)
            with self.assertRaises(RuntimeError): baseline.files(bundle)
            with self.assertRaises(RuntimeError): baseline.verify(directory, build / 'other')

    def test_comparator_upload_batch_keeps_all_dirty_bytes(self):
        probe = load('comparator_upload_batch', ROOT / 'scripts/comparator-probe.py')
        context = '\tif (useLocalScale())\n\twindow.updateSurface();'
        window = ('\tconst auto probeBegin = PerformanceTrace::now();\n'
                  '\tif (!SDL_UpdateTexture(...))\n'
                  '\tsdlPerformance::trace.record({PerformanceStage::UploadPreparation, sdlPerformance::stamp(),\n'
                  '\t    probeBegin, PerformanceTrace::now(), quint64(srcRect.w) * quint64(srcRect.h) * quint64(bpp)});\n')
        c, w = probe.upload_batch(context, window)
        self.assertIn('sdlPerformance::beginUploads();', c)
        self.assertIn('sdlPerformance::endUploads();', c)
        self.assertNotIn('PerformanceStage::UploadPreparation', w)
        self.assertIn('sdlPerformance::uploaded(quint64(srcRect.w) * quint64(srcRect.h) * quint64(bpp));', w)
        self.assertEqual(probe.upload_batch(c, w), (c, w))

    def test_comparator_final_event_does_not_consume_dialog_results(self):
        probe = load('comparator_final_event', ROOT / 'scripts/comparator-probe.py')
        anchor = ('\t\t\t\tconst int prc = SDL_PeepEvents(&windowEvent, 1, SDL_GETEVENT, SDL_EVENT_FIRST,\n'
                  '\t\t\t\t                               SDL_EVENT_USER_RETRY_DIALOG);')
        changed = probe.final_event_poll(anchor)
        self.assertIn('SDL_EVENT_USER_RETRY_DIALOG);', changed)
        self.assertIn('sdlPerformance::finalEvent, sdlPerformance::finalEvent);', changed)
        self.assertNotIn('SDL_EVENT_LAST', changed)
        self.assertEqual(probe.final_event_poll(changed), changed)
        with self.assertRaises(ValueError): probe.final_event_poll('missing')

    def test_performance_comparison_requires_native_codec_and_matched_geometry(self):
        runner = load('performance_runner', ROOT / 'scripts/test-performance.py')
        app = {'width': 1024, 'height': 768, 'dpr': 2, 'trace': {'codecs': [3], 'dropped': 0}}
        sdl = {'width': 1024, 'height': 768, 'dpr': 2, 'trace': {'codecs': [3], 'dropped': 0}}
        for report in (app, sdl):
            report.update(window_width=1024, window_height=768, pixel_width=2048, pixel_height=1536)
        self.assertEqual(runner.matching(app, sdl, 'cocoa'), [])
        sdl['pixel_width'] = 1024
        self.assertIn('native window or pixel geometry differs', runner.matching(app, sdl, 'cocoa'))
        sdl['pixel_width'] = 2048
        sdl['trace']['codecs'] = []
        self.assertIn('observed codec missing or different', runner.matching(app, sdl, 'cocoa'))
        sdl['width'] = 1200
        self.assertIn('remote geometry or DPR differs', runner.matching(app, sdl, 'cocoa'))
        self.assertIn('offscreen is not native presentation evidence', runner.matching(app, app, 'offscreen'))


if __name__ == '__main__':
    unittest.main()
