import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / (name + '.py'))
    value = importlib.util.module_from_spec(spec)
    with patch.dict(sys.modules, {'env': sys.modules.get('env') or _env()}):
        spec.loader.exec_module(value)
    return value


def _env():
    spec = importlib.util.spec_from_file_location('env', ROOT / 'scripts/env.py')
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value)
    return value


class ReleaseNames(unittest.TestCase):
    def test_installer_names_follow_project_version(self):
        release = module('release')
        version = release.version()
        self.assertRegex(version, r'^\d+\.\d+\.\d+$')
        self.assertEqual(release.release_name('windows', 'x64', '-setup.exe'),
                         'CMRemote-' + version + '-windows-x64-setup.exe')
        self.assertEqual(release.release_name('macos', 'arm64', '.dmg'), 'CMRemote-' + version + '-macos-arm64.dmg')
        self.assertEqual(release.release_name('linux', 'x86_64', '.flatpak'),
                         'CMRemote-' + version + '-linux-x86_64.flatpak')

    def test_desktop_entry_only_replaces_icon(self):
        release = module('release')
        original = (ROOT / 'app/assets/cmremote.desktop').read_text().splitlines()
        rewritten = release.desktop_entry(release.APP_ID).splitlines()
        self.assertEqual(len(original), len(rewritten))
        for before, after in zip(original, rewritten):
            self.assertEqual(after, 'Icon=' + release.APP_ID if before.startswith('Icon=') else before)

    def test_release_tools_are_pinned(self):
        artifacts = json.loads((ROOT / 'dependencies.lock.json').read_text())['artifacts']
        for key in ('appimagetool-linux-amd64', 'appimage-runtime-linux-amd64', 'llvm-mingw-linux-arm64',
                    'llvm-mingw-linux-amd64', 'qtbase-windows', 'qtshadertools-windows'):
            self.assertRegex(artifacts[key]['sha256'], r'^[0-9a-f]{64}$')
            self.assertTrue(artifacts[key]['url'].startswith('https://'))
            self.assertEqual(Path(artifacts[key]['file']).name, artifacts[key]['file'])

    def test_pinned_rejects_and_removes_mismatched_download(self):
        release = module('release')
        with tempfile.TemporaryDirectory(dir=ROOT / '.dev') as directory:
            root = Path(directory)
            (root / 'dependencies.lock.json').write_text(json.dumps({'artifacts': {'tool': {
                'url': 'https://example.invalid/tool', 'file': 'tool', 'sha256': '0' * 64}}}))
            cache = root / '.dev/linux/downloads'; cache.mkdir(parents=True)
            (cache / 'tool').write_bytes(b'tampered')
            with patch.object(release, 'ROOT', root):
                with self.assertRaises(RuntimeError):
                    release.pinned('tool')
            self.assertFalse((cache / 'tool').exists())

    def test_installer_script_rejects_quotes_in_paths(self):
        release = module('release')
        with tempfile.TemporaryDirectory(dir=ROOT / '.dev') as directory:
            package = Path(directory) / 'pkg"x'
            (package / 'platforms').mkdir(parents=True)
            (package / 'RemoteManager.exe').write_bytes(b'')
            (package / 'platforms/qwindows.dll').write_bytes(b'')
            with patch.object(release, 'run') as run, self.assertRaises(ValueError):
                release.windows_setup(package, Path(directory) / 'work', Path(directory) / 'out.exe')
            run.assert_not_called()


class WindowsPackage(unittest.TestCase):
    def test_system_dll_allowlist(self):
        package = module('package')
        for name in ('KERNEL32.dll', 'api-ms-win-crt-runtime-l1-1-0.dll', 'ws2_32.dll', 'd2d1.dll', 'winspool.drv'):
            self.assertTrue(package.windows_system_dll(name), name)
        # Toolchain/runtime and third-party DLLs must be shipped, never assumed present.
        for name in ('libc++.dll', 'libunwind.dll', 'Qt6Core.dll', 'libcrypto-3-x64.dll', 'libwinpthread-1.dll'):
            self.assertFalse(package.windows_system_dll(name), name)


class CrossEnvironment(unittest.TestCase):
    def test_windows_target_requires_linux_container(self):
        env = _env()
        with patch.dict(os.environ, {'RM_TARGET_PLATFORM': 'windows'}), \
             patch.object(env.platform, 'system', return_value='Darwin'):
            with self.assertRaises(SystemExit):
                env.environment()

    def test_cross_arch_is_macos_only(self):
        env = _env()
        with patch.dict(os.environ, {'RM_TARGET_ARCH': 'amd64'}), \
             patch.object(env.platform, 'system', return_value='Linux'), \
             patch.object(env.platform, 'machine', return_value='aarch64'):
            with self.assertRaises(SystemExit):
                env.environment()


if __name__ == '__main__':
    unittest.main()


class Versioning(unittest.TestCase):
    def test_set_version_updates_only_the_project_version(self):
        setter = module('set-version')
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            original = (ROOT / 'CMakeLists.txt').read_text()
            (root / 'CMakeLists.txt').write_text(original)
            current = setter.current(root)
            major, minor, patch_ = map(int, current.split('.'))
            newer = '%d.%d.%d' % (major, minor + 1, 0)
            self.assertEqual(setter.set_version('v' + newer, root), newer)
            updated = (root / 'CMakeLists.txt').read_text()
            self.assertIn('project(RemoteManager VERSION ' + newer + ' ', updated)
            self.assertEqual(updated.replace(newer, current, 1), original)
            for bad in ('1.2', '01.2.3', '1.2.3-beta', 'latest'):
                with self.assertRaises(SystemExit): setter.set_version(bad, root)
            # Releases never go backwards.
            with self.assertRaises(SystemExit): setter.set_version('0.0.0', root)

    def test_release_workflow_builds_every_installer_from_the_tag(self):
        workflow = (ROOT / '.github/workflows/release.yml').read_text()
        self.assertIn('workflow_dispatch:', workflow)
        self.assertIn('python3 scripts/set-version.py "$REQUESTED"', workflow)
        self.assertEqual(workflow.count('ref: ${{ needs.prepare.outputs.tag }}'), 4)
        # Published installers are optimized builds.
        self.assertEqual(workflow.count('package --release --artifact-name'), 3)
        for pattern in ('dist/macos/release/*.dmg', 'dist/linux/release/*.appimage', 'dist/linux/release/*.flatpak',
                        'dist/windows/release/*-setup.exe', 'dist/windows/release/*-portable.zip'):
            self.assertIn(pattern, workflow)
        release = module('release')
        for platform, arch, suffix in (('macos', 'arm64', '.dmg'), ('macos', 'x64', '.dmg'), ('linux', 'x86_64', '.appimage'),
                                       ('linux', 'x86_64', '.flatpak'), ('windows', 'x64', '-setup.exe')):
            name = release.release_name(platform, arch, suffix)
            self.assertTrue(name.startswith('CMRemote-' + release.version() + '-' + platform + '-' + arch))
        self.assertTrue((ROOT / '.github/release-notes.md').is_file())

    def test_app_reads_the_same_version(self):
        template = (ROOT / 'cmake/Version.h.in').read_text()
        self.assertIn('#define RM_VERSION "@PROJECT_VERSION@"', template)
        self.assertIn('configure_file(cmake/Version.h.in', (ROOT / 'CMakeLists.txt').read_text())


class SelfUpdate(unittest.TestCase):
    def test_setup_moves_the_installed_version_aside_and_rolls_back(self):
        script = module('release').NSIS_SCRIPT
        install = script[script.index('Section "Install"'):]
        install = install[:install.index('SectionEnd')]
        copy = install.index('File /r')
        # Never delete before copying: the installed version is moved aside, restored on failure.
        self.assertLess(install.index('Rename "$INSTDIR" "$INSTDIR.previous"'), copy)
        self.assertEqual(install[:copy].count('RMDir /r "$INSTDIR"'), 1)
        self.assertIn('IfFileExists "$INSTDIR.previous\\RemoteManager.exe" 0 staged\n    RMDir /r "$INSTDIR"', install)
        # A chosen folder without CMRemote in it is never moved or removed.
        self.assertIn('IfFileExists "$INSTDIR\\RemoteManager.exe" 0 copy', install)
        self.assertTrue(install.rstrip().endswith('RMDir /r "$INSTDIR.previous"'))
        failed = script[script.index('Function .onInstFailed'):]
        failed = failed[:failed.index('FunctionEnd')]
        self.assertIn('Rename "$INSTDIR.previous" "$INSTDIR"', failed)

    def test_only_published_release_packages_update_themselves(self):
        workflow = (ROOT / '.github/workflows/release.yml').read_text()
        self.assertEqual(workflow.count('--artifact-name "$NAME" --update-channel github'), 3)
        ci = (ROOT / '.github/workflows/ci.yml').read_text()
        self.assertNotIn('update-channel', ci)

    @unittest.skipUnless(os.name == 'posix', 'POSIX shell helpers')
    def test_appimage_helper_against_disposable_fixtures(self):
        import subprocess
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/test-auto-update.py'), '--suite', 'appimage'],
                                capture_output=True, text=True, timeout=300)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('canceled quit / crash before commit', result.stdout)
