import hashlib
import importlib.util
import json
from pathlib import Path
import platform
import plistlib
import shutil
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / (name + '.py'))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


class BrandingAssets(unittest.TestCase):
    def workspace(self):
        host = 'macos' if platform.system() == 'Darwin' else 'linux'
        base = ROOT / 'build' / host / 'test-data'; base.mkdir(parents=True, exist_ok=True)
        return tempfile.TemporaryDirectory(dir=base)

    def package(self):
        with patch.dict('sys.modules', {'env': module('env')}):
            return module('package')

    def test_supplied_source_and_every_generated_representation(self):
        branding = module('branding')
        manifest = branding.verify()
        self.assertEqual(manifest['source_sha256'], 'cec053a2e3b4297c26a6cd97acce309a7919da14a1ad48b6cef7df6391f5f70a')
        self.assertEqual(branding.png_size((branding.ASSETS / 'source.png').read_bytes()), (1254, 1254))
        self.assertEqual(len(manifest['files']), len(branding.SIZES) + len(branding.FAVICON_SIZES) + 4)

    def test_verification_rejects_missing_changed_symlinked_or_unlisted_assets(self):
        branding = module('branding')
        for damage in ('missing', 'changed', 'symlink', 'manifest'):
            with self.subTest(damage=damage), self.workspace() as directory:
                assets = Path(directory) / 'branding'; shutil.copytree(branding.ASSETS, assets)
                path = assets / 'cmremote-32.png'
                if damage == 'missing': path.unlink()
                elif damage == 'changed': path.write_bytes(path.read_bytes() + b'changed')
                elif damage == 'symlink':
                    path.unlink(); path.symlink_to(branding.ASSETS / path.name)
                else:
                    manifest = json.loads((assets / 'manifest.json').read_text())
                    manifest['files']['../outside.png'] = '0' * 64
                    (assets / 'manifest.json').write_text(json.dumps(manifest))
                with self.assertRaises(ValueError): branding.verify(assets)

    def test_ico_and_icns_bounds_and_frames_are_checked(self):
        branding = module('branding')
        ico = (branding.ASSETS / 'favicon.ico').read_bytes()
        for invalid in (ico[:6], ico[:-1], ico + b'extra', ico[:22] + b'changed' + ico[29:]):
            with self.assertRaises(ValueError): branding.validate_ico(invalid, branding.FAVICON_SIZES, branding.ASSETS)
        for invalid in (b'icns', b'icns\x00\x00\x00\x10' + b'ic07\x00\x00\x00\x00'):
            with self.assertRaises(ValueError): branding.validate_icns(invalid)

    def test_linux_stage_contains_icons_desktop_favicon_and_build_identity(self):
        branding = module('branding'); package = self.package()
        with self.workspace() as directory:
            destination = Path(directory) / 'artifact'
            source = Path(directory) / 'built-executable'; source.write_bytes(b'freshly built source fixture')
            with patch.dict('sys.modules', {'branding': branding}):
                package.stage_branding(destination, 'linux', source, ROOT / 'build/linux/arm64/app')
            for size in branding.SIZES:
                path = destination / ('share/icons/hicolor/' + str(size) + 'x' + str(size) + '/apps/cmremote.png')
                self.assertEqual(path.read_bytes(), (branding.ASSETS / ('cmremote-' + str(size) + '.png')).read_bytes())
            self.assertEqual((destination / 'share/branding/favicon.ico').read_bytes(), (branding.ASSETS / 'favicon.ico').read_bytes())
            desktop = (destination / 'share/applications/org.remotemanager.RemoteManager.desktop').read_text()
            self.assertIn('Name=CMRemote\n', desktop); self.assertIn('Icon=cmremote\n', desktop)
            self.assertTrue((destination / 'install-desktop.py').is_file())
            info = json.loads((destination / 'share/build-info.json').read_text())
            self.assertEqual(info['source_build'], 'build/linux/arm64/app')
            self.assertEqual(info['source_executable_sha256'], hashlib.sha256(source.read_bytes()).hexdigest())
            self.assertEqual(info['branding_source_sha256'], branding.SOURCE_SHA256)
            self.assertEqual(info['dependency_versions'], json.loads((ROOT / 'dependencies.lock.json').read_text())['versions'])
            self.assertNotIn(str(ROOT), json.dumps(info))
            self.assertEqual(info['version'], package.project_version())
            # Only the Release workflow marks a package as one that replaces itself.
            self.assertNotIn('update_channel', info)

    def test_release_package_identity_for_self_update(self):
        branding = module('branding'); package = self.package()
        self.assertEqual([package.package_architecture(h, a) for h, a in (('macos', 'arm64'), ('macos', 'amd64'), ('linux', 'amd64'), ('windows', 'amd64'))],
                         ['arm64', 'x64', 'x86_64', 'x64'])
        with self.workspace() as directory:
            destination = Path(directory) / 'artifact'
            source = Path(directory) / 'built-executable'; source.write_bytes(b'release fixture')
            with patch.dict('sys.modules', {'branding': branding}):
                with self.assertRaisesRegex(RuntimeError, 'Unknown update channel'):
                    package.stage_branding(destination, 'linux', source, ROOT / 'build/linux/arm64/app', 'x86_64', 'nightly')
                with self.assertRaisesRegex(RuntimeError, 'architecture and identity'):
                    package.stage_branding(destination, 'linux', source, ROOT / 'build/linux/arm64/app', None, 'github')
                package.stage_branding(destination, 'linux', source, ROOT / 'build/linux/arm64/app', 'x86_64', 'github')
            info = json.loads((destination / 'share/build-info.json').read_text())
            self.assertEqual((info['product'], info['version'], info['architecture'], info['update_channel']),
                             ('CMRemote', package.project_version(), 'x86_64', 'github'))

    def test_mac_stage_rejects_old_bundle_metadata_and_old_icons(self):
        branding = module('branding'); package = self.package()
        with self.workspace() as directory:
            destination = Path(directory) / 'Current.app'
            resources = destination / 'Contents/Resources'; resources.mkdir(parents=True)
            source = destination / 'Contents/MacOS/RemoteManager'; source.parent.mkdir(); source.write_bytes(b'new source')
            info_path = destination / 'Contents/Info.plist'
            info = dict(CFBundleIconFile='cmremote.icns', CFBundleName='CMRemote', CFBundleDisplayName='CMRemote', CFBundleExecutable='RemoteManager')
            info_path.write_bytes(plistlib.dumps(dict(info, CFBundleName='RemoteManager')))
            (resources / 'cmremote.icns').write_bytes(b'old icon')
            with patch.dict('sys.modules', {'branding': branding}):
                with self.assertRaisesRegex(RuntimeError, 'current CMRemote'): package.stage_branding(destination, 'macos', destination, ROOT / 'build/macos/app')
                info_path.write_bytes(plistlib.dumps(info))
                with self.assertRaisesRegex(RuntimeError, 'Bundle icon'): package.stage_branding(destination, 'macos', destination, ROOT / 'build/macos/app')
                shutil.copy2(branding.ASSETS / 'cmremote.icns', resources / 'cmremote.icns')
                package.stage_branding(destination, 'macos', destination, ROOT / 'build/macos/app')
                # A release bundle records the identifier the updater requires of its replacement.
                with self.assertRaisesRegex(RuntimeError, 'architecture and identity'):
                    package.stage_branding(destination, 'macos', destination, ROOT / 'build/macos/app', 'arm64', 'github')
                info_path.write_bytes(plistlib.dumps(dict(info, CFBundleIdentifier='org.remotemanager.RemoteManager')))
                package.stage_branding(destination, 'macos', destination, ROOT / 'build/macos/app', 'arm64', 'github')
            self.assertTrue((resources / 'build-info.json').is_file())
            identity = json.loads((resources / 'build-info.json').read_text())
            self.assertEqual((identity['bundle_identifier'], identity['architecture'], identity['update_channel']),
                             ('org.remotemanager.RemoteManager', 'arm64', 'github'))

    def test_upstream_notices_are_staged_without_unused_images_or_source(self):
        package = self.package()
        with self.workspace() as directory:
            destination = Path(directory)
            with patch.dict('sys.modules', {'ui_assets': module('ui_assets')}):
                package.stage_ui_notices(destination, 'linux')
            notices = destination / 'share/third-party/mremoteng-assets'
            self.assertEqual((notices / 'COPYING.txt').read_bytes(), (ROOT / 'vendor/mremoteng-assets/COPYING.txt').read_bytes())
            self.assertTrue((notices / 'CREDITS.md').is_file()); self.assertTrue((notices / 'manifest.json').is_file())
            self.assertTrue((notices / 'provenance.json').is_file())
            self.assertFalse(list(notices.rglob('*.png'))); self.assertFalse(list(notices.rglob('*.cs')))
            for name in ('Qt-LGPL-3.0.txt', 'Qt-GPL-3.0.txt', 'FreeRDP-LICENSE.txt', 'OpenSSL-LICENSE.txt',
                         'zlib-LICENSE.txt', 'README.txt'):
                self.assertTrue((destination / 'share/third-party' / name).is_file(), name)
            self.assertTrue((destination / 'share/LICENSE.txt').is_file())

    def test_optional_desktop_entry_uses_portable_launcher_and_updates_after_relocation(self):
        installer = module('install-desktop')
        with self.workspace() as directory:
            root = Path(directory); bundle = root / 'package with spaces'; bundle.mkdir()
            launcher = bundle / 'RemoteManager'; launcher.write_text('#!/bin/sh\nexit 0\n'); launcher.chmod(0o755)
            icon = bundle / 'share/icons/hicolor/256x256/apps/cmremote.png'; icon.parent.mkdir(parents=True); icon.write_bytes(b'fixture')
            template = bundle / 'share/applications' / installer.DESKTOP_ID; template.parent.mkdir(parents=True)
            template.write_bytes((ROOT / 'app/assets/cmremote.desktop').read_bytes())
            data_home = root / 'user-data'
            target = installer.install(bundle, data_home)
            text = target.read_text()
            self.assertIn('Exec="' + str(launcher) + '"\n', text)
            self.assertIn('Icon=' + str(icon) + '\n', text)
            self.assertNotIn('/bin/RemoteManager', text)
            moved = root / 'relocated package'; bundle.rename(moved)
            installer.install(moved, data_home)
            self.assertIn(str(moved / 'RemoteManager'), target.read_text())
            self.assertNotIn(str(bundle), target.read_text())
            target.write_text('[Desktop Entry]\nName=Unrelated\n')
            with self.assertRaisesRegex(ValueError, 'unrelated'): installer.install(moved, data_home)
        with self.assertRaises(ValueError): installer.exec_path(Path('/invalid\npath'))


if __name__ == '__main__':
    unittest.main()
