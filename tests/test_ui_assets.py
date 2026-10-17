"""Offline contracts for the curated UI assets and preview wrapper."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import ui_assets


def load_preview():
    spec = importlib.util.spec_from_file_location('preview_ui', ROOT / 'scripts/preview-ui.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class UiAssetsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve() / 'project'
        self.assets = self.root / 'vendor/mremoteng-assets'
        shutil.copytree(ROOT / 'vendor/mremoteng-assets', self.assets)
        (self.root / 'app/ui').mkdir(parents=True)
        self.qrc = self.root / 'app/ui/preview-icons.qrc'
        shutil.copyfile(ROOT / 'app/ui/preview-icons.qrc', self.qrc)

    def manifest(self):
        return json.loads((self.assets / 'manifest.json').read_bytes())

    def save_manifest(self, manifest):
        (self.assets / 'manifest.json').write_text(json.dumps(manifest))

    def invalid(self, pattern=None):
        if pattern:
            return self.assertRaisesRegex(ValueError, pattern)
        return self.assertRaises(ValueError)

    def test_current_subset_without_original_checkout(self):
        manifest = ui_assets.validate_assets(self.root)
        self.assertFalse((self.root / 'mRemoteNG').exists())
        self.assertFalse((self.root / 'app/ui/old-form-ui').exists())
        self.assertEqual(manifest['upstream']['commit'], ui_assets.UPSTREAM_COMMIT)
        self.assertEqual(len(manifest['assets']), 87)
        self.assertEqual(sum(len(r['aliases']) for r in manifest['assets']), 86)
        self.assertEqual(sum(r['destination'].startswith('Resources/') for r in manifest['assets']), 46)
        self.assertEqual(sum(r['destination'].startswith('Icons/') for r in manifest['assets']), 39)
        self.assertEqual({p.name for p in (self.assets / 'reference').iterdir()},
                         {'reconnect.png', 'external-tools.png'})
        self.assertFalse((self.assets / 'Resources/mRemoteNG_Icon.ico').exists())
        connection = next(r for r in manifest['assets'] if r['destination'] == 'Icons/mRemoteNG.ico')
        self.assertEqual(set(connection['aliases']),
                         {':/original/Connection.ico', ':/connection-icons/mRemoteNG.ico'})

    def test_stage_full_tree_and_repeat(self):
        target = self.root / 'build/assets/mremoteng-assets'
        manifest = ui_assets.stage_assets(target, self.root)
        ui_assets.stage_assets(target, self.root)
        for record in manifest['assets'] + manifest['notices']:
            self.assertEqual((target / record['destination']).read_bytes(),
                             (self.assets / record['destination']).read_bytes())
        provenance = json.loads((target / 'provenance.json').read_bytes())
        self.assertEqual(provenance['assets'], manifest['assets'])
        self.assertEqual(provenance['notices'], manifest['notices'])
        self.assertTrue(provenance['images_staged'])
        aliases = {a for r in provenance['assets'] for a in r['aliases']}
        self.assertIn(':/connection-icons/Linux.ico', aliases)
        self.assertNotIn(':/original/Linux.ico', aliases)
        self.assertEqual(json.loads((target / 'manifest.json').read_bytes()), manifest)

    def test_stage_package_notices_only(self):
        target = self.root / 'artifact/share/third-party/mremoteng-assets'
        manifest = ui_assets.stage_assets(target, self.root, include_images=False)
        self.assertEqual({p.name for p in target.iterdir()},
                         {'COPYING.txt', 'CREDITS.md', 'NOTICE.txt', 'manifest.json', 'provenance.json'})
        provenance = json.loads((target / 'provenance.json').read_bytes())
        self.assertFalse(provenance['images_staged'])
        self.assertEqual(provenance['assets'], manifest['assets'])
        self.assertIn('unresolved', provenance['licensing_warning'])

    def test_pinned_revision_and_warning_required(self):
        for key, value in [('upstream', {'url': ui_assets.UPSTREAM_URL, 'commit': '0' * 40}),
                           ('licensing_warning', 'cleared'), ('schema_version', 2)]:
            with self.subTest(key=key):
                manifest = self.manifest()
                original = manifest[key]
                manifest[key] = value
                self.save_manifest(manifest)
                with self.invalid('pinned source/license'):
                    ui_assets.validate_assets(self.root)
                manifest[key] = original
                self.save_manifest(manifest)

    def test_duplicate_json_keys_rejected(self):
        data = (self.assets / 'manifest.json').read_text()
        (self.assets / 'manifest.json').write_text(data.replace('"schema_version": 1',
                                                              '"schema_version": 1, "schema_version": 1'))
        with self.invalid('Duplicate asset manifest key'):
            ui_assets.validate_assets(self.root)

    def test_image_and_notice_hashes_required(self):
        for name in ['Resources/Run_16x.png', 'COPYING.txt', 'CREDITS.md', 'NOTICE.txt']:
            with self.subTest(name=name):
                path = self.assets / name
                data = path.read_bytes()
                path.write_bytes(data + b'changed')
                with self.invalid('checksum mismatch'):
                    ui_assets.validate_assets(self.root)
                path.write_bytes(data)

    def test_format_checked_even_with_updated_hash(self):
        manifest = self.manifest()
        record = manifest['assets'][0]
        path = self.assets / record['destination']
        path.write_bytes(b'not an icon')
        record['sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
        self.save_manifest(manifest)
        with self.invalid('standalone ICO'):
            ui_assets.validate_assets(self.root)

    def test_manifest_paths_cannot_escape(self):
        for name in ['../escape.png', '/tmp/escape.png', 'Icons/../escape.ico',
                     'Icons\\escape.ico', 'Icons//escape.ico']:
            with self.subTest(name=name):
                manifest = self.manifest()
                original = manifest['assets'][0]['destination']
                manifest['assets'][0]['destination'] = name
                self.save_manifest(manifest)
                with self.invalid('Unsafe asset path'):
                    ui_assets.validate_assets(self.root)
                manifest['assets'][0]['destination'] = original
                self.save_manifest(manifest)
        self.assertFalse((self.root / 'escape.png').exists())

    def test_duplicate_destinations_rejected(self):
        manifest = self.manifest()
        manifest['assets'].append(manifest['assets'][0])
        self.save_manifest(manifest)
        with self.invalid('Duplicate asset destination'):
            ui_assets.validate_assets(self.root)

    def test_reference_names_and_entries_are_exact(self):
        manifest = self.manifest()
        record = next(r for r in manifest['assets'] if r['destination'] == 'reference/reconnect.png')
        record['resource_entry'] = '$this.Icon'
        self.save_manifest(manifest)
        with self.invalid('extracted reference provenance'):
            ui_assets.validate_assets(self.root)

    def test_missing_file_rejected(self):
        (self.assets / 'reference/reconnect.png').unlink()
        with self.invalid('Missing or non-regular'):
            ui_assets.validate_assets(self.root)

    def test_extra_files_and_empty_directories_rejected(self):
        extra = self.assets / 'extra.png'
        extra.write_bytes(b'not declared')
        with self.invalid('files differ from manifest'):
            ui_assets.validate_assets(self.root)
        extra.unlink()
        (self.assets / 'source-code').mkdir()
        with self.invalid('Unexpected curated asset directory'):
            ui_assets.validate_assets(self.root)

    def test_file_symlink_rejected_even_inside_tree(self):
        path = self.assets / 'reference/reconnect.png'
        path.unlink()
        path.symlink_to('external-tools.png')
        with self.invalid('Symlink'):
            ui_assets.validate_assets(self.root)

    def test_directory_symlink_rejected(self):
        directory = self.assets / 'Icons'
        outside = self.root / 'outside'
        directory.rename(outside)
        directory.symlink_to(outside, target_is_directory=True)
        with self.invalid('Symlink'):
            ui_assets.validate_assets(self.root)

    def test_catalog_symlink_rejected(self):
        copy = self.root / 'catalog.qrc'
        self.qrc.rename(copy)
        self.qrc.symlink_to(copy)
        with self.invalid('Symlink'):
            ui_assets.validate_assets(self.root)

    def test_catalog_namespace_and_aliases_must_match(self):
        text = self.qrc.read_text()
        self.qrc.write_text(text.replace('prefix="/connection-icons"', 'prefix="/original"'))
        with self.invalid('namespace'):
            ui_assets.validate_assets(self.root)
        self.qrc.write_text(text.replace('alias="Linux.ico"', 'alias="Other.ico"'))
        with self.invalid('differs from manifest'):
            ui_assets.validate_assets(self.root)

    def test_manifest_provenance_uses_correct_namespace(self):
        manifest = self.manifest()
        record = next(r for r in manifest['assets'] if r['destination'] == 'Icons/Linux.ico')
        record['aliases'] = [':/original/Linux.ico']
        self.save_manifest(manifest)
        with self.invalid('reviewed namespaces'):
            ui_assets.validate_assets(self.root)

    def test_catalog_path_cannot_escape_tree(self):
        text = self.qrc.read_text().replace('../../vendor/mremoteng-assets/Icons/Admin.ico',
                                            '../../outside/Admin.ico')
        self.qrc.write_text(text)
        with self.invalid('escapes curated asset tree'):
            ui_assets.validate_assets(self.root)

    def test_catalog_paths_cannot_hide_symlink_parent_traversal(self):
        outside = self.root / 'outside'
        outside.mkdir()
        (self.root / 'linked').symlink_to(outside, target_is_directory=True)
        text = self.qrc.read_text().replace('../../vendor/mremoteng-assets/Icons/Admin.ico',
                                            '../../linked/../vendor/mremoteng-assets/Icons/Admin.ico')
        self.qrc.write_text(text)
        with self.invalid('Non-canonical resource catalog path'):
            ui_assets.validate_assets(self.root)

    def test_catalog_duplicate_alias_rejected(self):
        text = self.qrc.read_text().replace('alias="Linux.ico"', 'alias="Log.ico"')
        self.qrc.write_text(text)
        with self.invalid('Duplicate resource alias'):
            ui_assets.validate_assets(self.root)

    def test_catalog_missing_alias_rejected(self):
        tree = ET.parse(self.qrc)
        group = tree.getroot().findall('qresource')[1]
        group.remove(group[0])
        tree.write(self.qrc)
        with self.invalid('namespace counts'):
            ui_assets.validate_assets(self.root)

    def test_catalog_xml_declarations_rejected(self):
        self.qrc.write_text('<!DOCTYPE RCC><RCC/>')
        with self.invalid('declarations'):
            ui_assets.validate_assets(self.root)

    def test_stage_validates_before_creating_destination(self):
        (self.assets / 'COPYING.txt').write_bytes(b'changed')
        target = self.root / 'build/assets'
        with self.invalid('checksum mismatch'):
            ui_assets.stage_assets(target, self.root)
        self.assertFalse(target.exists())

    def test_stage_refuses_source_overlap(self):
        for target in [self.assets, self.assets / 'stage', self.root]:
            with self.subTest(target=target), self.invalid('overlaps source'):
                ui_assets.stage_assets(target, self.root)

    def test_stage_does_not_clean_unexpected_files(self):
        target = self.root / 'stage'
        target.mkdir()
        old = target / 'main.ico'
        old.write_bytes(b'historical generated image')
        with self.invalid('Unexpected staged asset path'):
            ui_assets.stage_assets(target, self.root)
        self.assertEqual(old.read_bytes(), b'historical generated image')
        self.assertFalse((target / 'manifest.json').exists())

    def test_stage_rejects_symlink_destination_and_parent(self):
        outside = self.root / 'outside'
        outside.mkdir()
        link = self.root / 'linked'
        link.symlink_to(outside, target_is_directory=True)
        for target in [link, link / 'stage']:
            with self.subTest(target=target), self.invalid('Symlink'):
                ui_assets.stage_assets(target, self.root)
        self.assertEqual(list(outside.iterdir()), [])

    def test_stage_rejects_existing_file_symlink(self):
        target = self.root / 'stage'
        target.mkdir()
        outside = self.root / 'outside.txt'
        outside.write_bytes(b'preserve')
        (target / 'COPYING.txt').symlink_to(outside)
        with self.invalid('Symlink'):
            ui_assets.stage_assets(target, self.root, include_images=False)
        self.assertEqual(outside.read_bytes(), b'preserve')

    def test_stage_replaces_hardlink_without_changing_external_file(self):
        target = self.root / 'stage'
        target.mkdir()
        outside = self.root / 'outside.txt'
        outside.write_bytes(b'preserve')
        os.link(outside, target / 'COPYING.txt')
        ui_assets.stage_assets(target, self.root, include_images=False)
        self.assertEqual(outside.read_bytes(), b'preserve')
        self.assertEqual((target / 'COPYING.txt').read_bytes(), (self.assets / 'COPYING.txt').read_bytes())

    def test_image_validation_limits(self):
        png = b'\x89PNG\r\n\x1a\n' + b'\x00\x00\x00\rIHDR' + struct.pack('>II', 16, 16)
        ico = b'\x00\x00\x01\x00\x01\x00' + b'\x01\x01\x00\x00\x01\x00\x20\x00' + struct.pack('<II', 4, 22) + b'data'
        ui_assets.validate_image(png, 'image.png')
        ui_assets.validate_image(ico, 'image.ico')
        for data, name in [(b'', 'a.png'), (b'x' * (4 * 1024 * 1024 + 1), 'a.png'),
                           (png[:20], 'a.png'), (png[:16] + struct.pack('>II', 2049, 1), 'a.png'),
                           (ico[:6], 'a.ico'), (ico[:-1], 'a.ico'), (png, 'image.resx')]:
            with self.subTest(name=name, size=len(data)), self.invalid():
                ui_assets.validate_image(data, name)

    def test_preview_preserves_build_test_run_capture_commands(self):
        preview = load_preview()
        build_root = self.root / 'build'
        for command, options, extra in [
                ('build', [], None), ('test', [], 'ctest'),
                ('run', ['--smoke', '--menu', 'session'], 'UiPreview'),
                ('capture', ['--offscreen', '--menu', 'tree'], 'UiPreview')]:
            with self.subTest(command=command), patch.object(preview, 'ROOT', self.root), \
                    patch.object(preview, 'environment', return_value={'RM_BUILD_DIR': str(build_root)}), \
                    patch.object(preview.subprocess, 'run') as run, \
                    patch.object(sys, 'argv', ['preview-ui.py', command, *options]):
                preview.main()
                calls = run.call_args_list
                self.assertEqual(calls[0].args[0][:2], ['cmake', '-S'])
                self.assertEqual(calls[1].args[0][:2], ['cmake', '--build'])
                self.assertEqual(len(calls), 2 if extra is None else 3)
                env = calls[0].kwargs['env']
                reference = build_root / 'ui-preview/assets/mremoteng-assets/reference'
                self.assertEqual(env['RM_PREVIEW_ASSET_DIR'], str(reference))
                self.assertTrue((reference / 'reconnect.png').is_file())
                self.assertFalse((reference / 'main.ico').exists())
                self.assertEqual(calls[0].kwargs['cwd'], self.root)
                if extra:
                    self.assertEqual(Path(calls[2].args[0][0]).name, extra)
                if command in ('test', 'capture'):
                    self.assertEqual(env['QT_QPA_PLATFORM'], 'offscreen')
                if command == 'run':
                    self.assertEqual(calls[2].args[0][1:], ['--smoke', '--menu', 'session'])
                if command == 'capture':
                    self.assertEqual(calls[2].args[0][-2], '--screenshot')
                    self.assertTrue(calls[2].args[0][-1].endswith('/preview-offscreen-tree.png'))


if __name__ == '__main__':
    unittest.main()
