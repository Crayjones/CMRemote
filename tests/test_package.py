import importlib.util
import io
import os
from pathlib import Path
import platform
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def runner_module():
    spec = importlib.util.spec_from_file_location('package_runtime', ROOT / 'scripts/test-package.py')
    runner = importlib.util.module_from_spec(spec); spec.loader.exec_module(runner)
    return runner


class PackageRuntime(unittest.TestCase):
    def workspace(self):
        host = 'macos' if platform.system() == 'Darwin' else 'linux'
        base = ROOT / 'build' / host / 'test-data'; base.mkdir(parents=True, exist_ok=True)
        return tempfile.TemporaryDirectory(dir=base)

    def test_package_probe_environment_excludes_sdk_and_backend_overrides(self):
        runner = runner_module(); before = dict(os.environ)
        with self.workspace() as directory:
            temp = Path(directory)
            injected = dict(LD_LIBRARY_PATH='/sdk/lib', QT_PLUGIN_PATH='/sdk/plugins',
                            QT_WIDGETS_RHI_BACKEND='null', OPENSSL_MODULES='/system/modules',
                            RM_RDP_TEST_PASSWORD='synthetic-secret')
            with patch.dict(os.environ, injected): env = runner.runtime_environment(temp)
            for key in injected: self.assertNotIn(key, env)
            self.assertEqual(env['LIBGL_ALWAYS_SOFTWARE'], '1')
            self.assertEqual(env.get('HOME'), before.get('HOME'))
            for key in ('TMPDIR', 'XDG_DATA_HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME'):
                self.assertTrue(Path(env[key]).is_relative_to(temp))
        self.assertEqual(dict(os.environ), before)

    def test_package_probe_relocates_archive_and_rejects_missing_runtimes(self):
        runner = runner_module()
        with self.workspace() as directory:
            root = Path(directory); archive_path = root / 'package.tar.gz'
            with tarfile.open(archive_path, 'w:gz') as archive:
                for name in runner.REQUIRED:
                    entry = tarfile.TarInfo('Artifact/' + name); entry.size = 5
                    archive.addfile(entry, io.BytesIO(b'owned'))
            bundle = runner.relocate(archive_path, root / 'relocated')
            self.assertEqual(bundle, root / 'relocated/Artifact')
            self.assertTrue(all((bundle / name).is_file() for name in runner.REQUIRED))
            (bundle / 'lib/ossl-modules/legacy.so').unlink()
            with self.assertRaisesRegex(RuntimeError, 'Missing required package runtime'):
                runner.relocate(bundle, root / 'invalid')

    def test_package_probe_rejects_archive_traversal_and_links(self):
        runner = runner_module()
        for name, kind in [('Artifact/../../outside', tarfile.REGTYPE),
                           ('/outside', tarfile.REGTYPE), ('Artifact/link', tarfile.SYMTYPE),
                           ('Artifact/hardlink', tarfile.LNKTYPE)]:
            with self.subTest(name=name), self.workspace() as directory:
                root = Path(directory); archive_path = root / 'unsafe.tar.gz'
                with tarfile.open(archive_path, 'w:gz') as archive:
                    entry = tarfile.TarInfo(name); entry.type = kind; entry.linkname = '../outside'
                    archive.addfile(entry)
                with self.assertRaisesRegex(RuntimeError, 'Unsafe Linux package archive'):
                    runner.relocate(archive_path, root / 'relocated')
                self.assertFalse((root / 'outside').exists())

    def test_package_probe_launcher_uses_shipping_environment(self):
        runner = runner_module(); before = dict(os.environ)
        with self.workspace() as directory:
            bundle = Path(directory) / 'path with spaces'; bundle.mkdir()
            (bundle / 'RemoteManager').write_text(
                '#!/bin/sh\nset -eu\nDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
                'export QT_PLUGIN_PATH="$DIR/plugins"\nexport OPENSSL_MODULES="$DIR/lib/ossl-modules"\n'
                'exec "$DIR/bin/RemoteManager" "$@"\n')
            wrapper = runner.launcher_wrapper(bundle)
            result = runner.checked_run([wrapper, '/usr/bin/env'], runner.runtime_environment(Path(directory)))
            self.assertIn('OPENSSL_MODULES=' + str(bundle / 'lib/ossl-modules'), result)
            self.assertIn('QT_PLUGIN_PATH=' + str(bundle / 'plugins'), result)
            (bundle / 'RemoteManager').write_text('unrecognized launcher')
            with self.assertRaisesRegex(RuntimeError, 'Unrecognized package launcher'):
                runner.launcher_wrapper(bundle)
        self.assertEqual(dict(os.environ), before)

    def test_package_probe_rejects_skipped_or_incomplete_rhi_evidence(self):
        runner = runner_module()
        passed = ''.join('PASS   : Smoke::' + case + '()\n' for case in runner.RHI_CASES)
        totals = 'Totals: 6 passed, 0 failed, 0 skipped, 10ms\n'
        finished = '********* Finished testing of Smoke *********\n'
        self.assertTrue(runner.accepted_rhi_report(passed + totals + finished))
        for report in (passed + totals, totals + finished,
                       passed + totals.replace('0 skipped', '1 skipped') + finished,
                       passed.replace('PASS', 'FAIL', 1) + totals + finished,
                       passed + totals.replace('0 failed', '1 failed') + finished):
            self.assertFalse(runner.accepted_rhi_report(report))


if __name__ == '__main__': unittest.main()
