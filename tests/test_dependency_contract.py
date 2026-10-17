import copy
import importlib.util
import json
import os
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('verify', ROOT / 'scripts/verify-dependencies.py')
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


class DependencyContract(unittest.TestCase):
    def test_pinned_source_checksums(self):
        import hashlib
        data = b'pinned artifact'
        checksum = hashlib.sha256(data).hexdigest()
        verify.verify_bytes(data, checksum)
        with self.assertRaises(ValueError):
            verify.verify_bytes(data + b'x', checksum)

    def test_freerdp_api_contract(self):
        sources = {p: '\n'.join(symbols) for p, symbols in verify.API.items()}
        verify.verify_api(sources)
        broken = copy.deepcopy(sources)
        broken['include/freerdp/client/disp.h'] = 'DispClientContext DisplayControlCaps'
        with self.assertRaises(ValueError):
            verify.verify_api(broken)

    def test_performance_source_api_and_license_contract(self):
        sources = {key: {path: '\n'.join(symbols) for path, symbols in files.items()}
                   for key, files in verify.PERFORMANCE_API.items()}
        verify.verify_performance_api(sources)
        broken = copy.deepcopy(sources)
        broken['openh264']['codec/api/wels/codec_api.h'] = 'WelsCreateDecoder'
        with self.assertRaises(ValueError): verify.verify_performance_api(broken)
        broken = copy.deepcopy(sources)
        del broken['freetype_comparator']['docs/FTL.TXT']
        with self.assertRaises(ValueError): verify.verify_performance_api(broken)

    def test_cache_containment(self):
        self.assertEqual(verify.cache_dir('macos'), ROOT / '.dev/macos/downloads')
        self.assertEqual(verify.cache_dir('linux'), ROOT / '.dev/linux/downloads')
        for platform in ('../../etc', 'windows', '/tmp'):
            with self.assertRaises(ValueError):
                verify.cache_dir(platform)

    def test_cxx_toolchain_contract(self):
        lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
        self.assertEqual(lock['versions']['cxx'], '1.0.202')
        self.assertEqual(lock['artifacts']['cxx-build']['file'], 'cxx-build.crate')
        self.assertGreaterEqual(tuple(map(int, lock['versions']['rust'].split('.'))), (1, 88, 0))

    def test_qt_minimal_archive_contract(self):
        lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
        qt_archives = [k for k in lock['artifacts'] if k.startswith(('qtbase-', 'qtwayland-', 'icu-', 'qtshadertools-'))]
        self.assertIn('qtbase-macos', qt_archives)
        self.assertIn('qtwayland-linux-arm64', qt_archives)
        self.assertIn('qtshadertools-macos', qt_archives)
        self.assertFalse(any('declarative' in k or 'webengine' in k for k in lock['artifacts']))

    def test_comparator_sources_are_pinned_and_separate(self):
        lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
        for key, version in (('sdl3', '3.2.26'), ('sdl3_ttf', '3.2.2'),
                             ('freetype_comparator', '2.13.3'), ('openh264', '2.6.0')):
            self.assertEqual(lock['versions'][key], version)
            artifact = lock['artifacts'][key]
            self.assertTrue(artifact['url'].startswith('https://'))
            self.assertEqual(len(artifact['sha256']), 64)
            self.assertEqual(Path(artifact['file']).name, artifact['file'])
        spec = importlib.util.spec_from_file_location('comparator', ROOT / 'scripts/bootstrap-comparator.py')
        comparator = importlib.util.module_from_spec(spec); spec.loader.exec_module(comparator)
        prefix = ROOT / '.dev/macos/comparator/prefix'
        options = comparator.options('freerdp', prefix, ROOT / '.dev/macos/prefix')
        self.assertIn('-DWITH_OPENH264=OFF', options)
        self.assertIn('-DWITH_CLIENT_SDL3=ON', options)
        self.assertIn('-DWITH_CLIENT_SDL2=OFF', options)
        self.assertIn('-DWITH_SERVER=OFF', options)
        self.assertIn('-DCMAKE_INSTALL_PREFIX=' + str(prefix), options)
        self.assertNotIn('-DCMAKE_INSTALL_PREFIX=' + str(ROOT / '.dev/macos/prefix'), options)
        freetype = comparator.options('freetype_comparator', prefix, ROOT / '.dev/macos/prefix')
        for dependency in ('ZLIB', 'PNG', 'BZIP2', 'HARFBUZZ', 'BROTLI'):
            self.assertIn('-DFT_DISABLE_' + dependency + '=ON', freetype)
        ttf = comparator.options('sdl3_ttf', prefix, ROOT / '.dev/macos/prefix')
        self.assertIn('-DSDLTTF_VENDORED=OFF', ttf)
        self.assertIn('-DSDLTTF_HARFBUZZ=OFF', ttf)
        self.assertIn('-DSDLTTF_PLUTOSVG=OFF', ttf)

    def test_decoder_dependency_contract(self):
        spec = importlib.util.spec_from_file_location('bootstrap', ROOT / 'scripts/bootstrap.py')
        bootstrap = importlib.util.module_from_spec(spec); spec.loader.exec_module(bootstrap)
        prefix = ROOT / '.dev/macos/prefix'
        options = bootstrap.freerdp_options(prefix, 'macos')
        for flag in ('WITH_OPENH264=ON', 'WITH_OPENH264_LOADING=ON', 'WITH_FFMPEG=OFF',
                     'WITH_VIDEO_FFMPEG=OFF', 'WITH_DSP_FFMPEG=OFF', 'WITH_MEDIA_FOUNDATION=OFF',
                     'WITH_MEDIACODEC=OFF', 'WITH_FFMPEG_HWACCEL=OFF'):
            self.assertIn('-D' + flag, options)
        self.assertIn('-DOPENH264_INCLUDE_DIR=' + str(prefix / 'include'), options)
        self.assertIn('-DOPENH264_LIBRARY=' + str(prefix / 'lib/libopenh264.dylib'), options)
        self.assertNotIn('-DWITH_OPENH264=OFF', options)
        linux = bootstrap.freerdp_options(ROOT / '.dev/linux/prefix', 'linux')
        self.assertIn('-DOPENH264_LIBRARY=' + str(ROOT / '.dev/linux/prefix/lib/libopenh264.so'), linux)
        self.assertIn('-DWITH_UNICODE_BUILTIN=ON', linux)
        self.assertNotIn('-DWITH_OPENH264_LOADING=OFF', options)

    def test_freerdp_loads_only_the_app_supplied_openh264(self):
        import tempfile
        spec = importlib.util.spec_from_file_location('bootstrap', ROOT / 'scripts/bootstrap.py')
        bootstrap = importlib.util.module_from_spec(spec); spec.loader.exec_module(bootstrap)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory); codec = source / 'libfreerdp/codec'; codec.mkdir(parents=True)
            original = ('#include <winpr/winpr.h>\n' + 'static const char* openh264_library_names[] = {};\n' +
                        bootstrap.OPENH264_SETTER_ANCHOR + 'void* c, int l, const char* m) {}\n' +
                        'static BOOL openh264_init(H264_CONTEXT* h264) {\n' + bootstrap.OPENH264_LOADER_ORIGINAL + '}\n')
            (codec / 'h264_openh264.c').write_text(original)
            bootstrap.patch_freerdp(source); patched = (codec / 'h264_openh264.c').read_text()
            bootstrap.patch_freerdp(source)
            self.assertEqual((codec / 'h264_openh264.c').read_text(), patched)
            self.assertIn('FREERDP_API void freerdp_openh264_set_library(const char* path)', patched)
            self.assertNotIn(bootstrap.OPENH264_LOADER_ORIGINAL, patched)
            self.assertIn('#include <stdatomic.h>', patched)
            (codec / 'h264_openh264.c').write_text('unrelated'); (codec / 'h264_openh264.c.orig').unlink()
            with self.assertRaisesRegex(RuntimeError, 'pinned source'): bootstrap.patch_freerdp(source)

    def test_decoderless_sdk_cannot_satisfy_reuse_contract(self):
        import tempfile
        spec = importlib.util.spec_from_file_location('bootstrap', ROOT / 'scripts/bootstrap.py')
        bootstrap = importlib.util.module_from_spec(spec); spec.loader.exec_module(bootstrap)
        base = ROOT / 'build' / ('macos' if os.uname().sysname == 'Darwin' else 'linux') / 'test-data'
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as directory:
            prefix = Path(directory)
            header = prefix / 'include/freerdp3/freerdp/freerdp.h'
            header.parent.mkdir(parents=True); header.write_text('legacy header')
            contract = {'version': '3.32.1', 'decoder': '2.6.0', 'options': ['WITH_OPENH264=ON']}
            files = ['include/freerdp3/freerdp/freerdp.h', 'lib/libfreerdp3.dylib']
            self.assertFalse(bootstrap.dependency_current(prefix, 'freerdp', contract, files))
            library = prefix / files[1]; library.parent.mkdir(); library.write_bytes(b'owned synthetic library')
            bootstrap.record_dependency(prefix, 'freerdp', contract, files)
            self.assertTrue(bootstrap.dependency_current(prefix, 'freerdp', contract, files))
            changed = copy.deepcopy(contract); changed['decoder'] = 'different'
            self.assertFalse(bootstrap.dependency_current(prefix, 'freerdp', changed, files))
            library.write_bytes(b'changed synthetic library')
            self.assertFalse(bootstrap.dependency_current(prefix, 'freerdp', contract, files))
            library.unlink(); library.symlink_to(ROOT / 'CMakeLists.txt')
            with self.assertRaisesRegex(ValueError, 'escapes'):
                bootstrap.record_dependency(prefix, 'freerdp', contract, files)

    def test_ssh_dependencies_are_exact_and_licensed(self):
        lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
        for key, version in (('russh', '0.64.1'), ('tokio', '1.48.0'), ('libvterm', '0.3.3')):
            self.assertEqual(lock['versions'][key], version)
        for key in ('russh', 'libvterm'):
            artifact = lock['artifacts'][key]
            self.assertTrue(artifact['url'].startswith('https://'))
            self.assertEqual(len(artifact['sha256']), 64)
        sources = {key: {path: '\n'.join(symbols) for path, symbols in files.items()}
                   for key, files in verify.SSH_API.items()}
        verify.verify_ssh_api(sources)
        broken = copy.deepcopy(sources)
        del broken['libvterm']['LICENSE']
        with self.assertRaises(ValueError): verify.verify_ssh_api(broken)

    def test_ssh_frontend_dependencies_are_not_rdp_optional(self):
        cmake = (ROOT / 'CMakeLists.txt').read_text()
        before_rdp = cmake.split('if(RM_WITH_RDP)', 1)[0]
        self.assertIn('find_package(VTerm 0.3.3 EXACT REQUIRED CONFIG)', before_rdp)
        self.assertIn('Qt6::Network', before_rdp)
        self.assertIn('VTerm::vterm', before_rdp)
        cargo = (ROOT / 'core/Cargo.toml').read_text()
        self.assertIn('russh = { version = "=0.64.1", default-features = false, features = ["ring", "rsa"] }', cargo)
        self.assertIn('tokio = { version = "=1.48.0"', cargo)
        resolver = (ROOT / 'scripts/resolve-dependencies.py').read_text()
        self.assertNotIn('russh_planned', resolver)
        self.assertNotIn('alacritty_terminal_planned', resolver)
        self.assertIn('rust-notice-', resolver)

    def test_libvterm_static_build_contract(self):
        adapter = (ROOT / 'cmake/libvterm/CMakeLists.txt').read_text()
        for token in ('STATIC', 'C_STANDARD 99', 'POSITION_INDEPENDENT_CODE ON',
                      'src/keyboard.c', 'src/screen.c', 'src/state.c',
                      'include/vterm.h', 'share/licenses/libvterm', 'VTermConfig.cmake'):
            self.assertIn(token, adapter)
        self.assertNotIn('FetchContent', adapter)
        self.assertNotIn('/usr/local', adapter)
        dockerfile = (ROOT / 'Dockerfile').read_text()
        self.assertIn('COPY cmake/libvterm/ ./cmake/libvterm/', dockerfile)
        self.assertLess(dockerfile.index('COPY cmake/libvterm/'), dockerfile.index('scripts/bootstrap.py --platform linux'))

    def test_ssh_linux_image_supplies_owned_key_fixture_generator(self):
        dev = (ROOT / 'Dockerfile').read_text().split(' AS rdp-fixture', 1)[0]
        self.assertIn('openssh-client', dev)
        self.assertIn('--no-install-recommends', dev)
        self.assertIn('dpkg-query -W > .dev/linux/native-packages.txt', dev)

    def test_docker_runtime_uid_has_container_account_without_home_override(self):
        dev = (ROOT / 'Dockerfile').read_text().split(' AS rdp-fixture', 1)[0]
        self.assertIn('ARG LOCAL_UID=1001', dev)
        self.assertIn('ARG LOCAL_GID=1001', dev)
        self.assertIn('getent passwd "$LOCAL_UID"', dev)
        self.assertIn('getent group "$LOCAL_GID"', dev)
        self.assertIn('USER ${LOCAL_UID}:${LOCAL_GID}', dev)
        self.assertNotIn('ENV HOME=', dev)
        wrapper = (ROOT / 'scripts/docker.py').read_text()
        self.assertIn("'LOCAL_UID=' + env['LOCAL_UID']", wrapper)
        self.assertIn("'LOCAL_GID=' + env['LOCAL_GID']", wrapper)
        compose = (ROOT / 'compose.yaml').read_text()
        self.assertIn('LOCAL_UID: "${LOCAL_UID:-1001}"', compose)
        self.assertIn('LOCAL_GID: "${LOCAL_GID:-1001}"', compose)

    def test_libvterm_existing_sdk_upgrade(self):
        import tempfile
        from unittest.mock import patch
        spec = importlib.util.spec_from_file_location('bootstrap', ROOT / 'scripts/bootstrap.py')
        bootstrap = importlib.util.module_from_spec(spec); spec.loader.exec_module(bootstrap)
        base = ROOT / 'build' / ('macos' if os.uname().sysname == 'Darwin' else 'linux') / 'test-data'
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as directory:
            prefix = Path(directory) / 'prefix'; prefix.mkdir()
            source = Path(directory) / 'source'; source.mkdir()
            build = Path(directory) / 'build'
            contract = {'version': '0.3.3', 'arch': 'arm64', 'source': 'owned-source'}
            def install(*args, **kwargs):
                if '--install' in args:
                    for name in bootstrap.LIBVTERM_FILES:
                        path = prefix / name; path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_bytes(b'owned libvterm output')
            with patch.object(bootstrap, 'run', side_effect=install) as run, \
                 patch.object(bootstrap, 'validate_libvterm_sdk') as probe:
                bootstrap.install_libvterm(source, build, prefix, 'cmake', 2, {}, contract)
                self.assertEqual(run.call_count, 3)
                bootstrap.install_libvterm(source, build, prefix, 'cmake', 2, {}, contract)
                self.assertEqual(run.call_count, 3)
                self.assertEqual(probe.call_count, 2)
                (prefix / 'lib/libvterm.a').write_bytes(b'changed library')
                bootstrap.install_libvterm(source, build, prefix, 'cmake', 2, {}, contract)
                self.assertEqual(run.call_count, 6)

    def test_https_fetch_fallback_keeps_tls_verification(self):
        from unittest.mock import patch
        import ssl
        import urllib.error
        data = b'owned synthetic download'
        error = urllib.error.URLError(ssl.SSLError('TLSV1_ALERT_PROTOCOL_VERSION'))
        with patch.object(verify.urllib.request, 'urlopen', side_effect=error), \
             patch.object(verify.subprocess, 'run') as curl:
            curl.return_value.stdout = data
            self.assertEqual(verify.download('https://fixture.invalid/source'), data)
            command = curl.call_args.args[0]
            self.assertIn('--proto', command)
            self.assertIn('=https', command)
            self.assertIn('--max-time', command)
            self.assertNotIn('--insecure', command)
            self.assertNotIn('-k', command)
        with patch.object(verify.urllib.request, 'urlopen',
                          side_effect=urllib.error.URLError(ssl.SSLCertVerificationError())), \
             patch.object(verify.subprocess, 'run') as curl:
            with self.assertRaises(urllib.error.URLError):
                verify.download('https://fixture.invalid/source')
            curl.assert_not_called()
        with self.assertRaises(ValueError): verify.download('http://fixture.invalid/source')

    def test_libvterm_package_contains_notice_without_sdk_runtime(self):
        import tempfile
        from unittest.mock import patch
        import sys
        sys.path.insert(0, str(ROOT / 'scripts'))
        try:
            spec = importlib.util.spec_from_file_location('package', ROOT / 'scripts/package.py')
            package = importlib.util.module_from_spec(spec); spec.loader.exec_module(package)
        finally:
            sys.path.pop(0)
        spec = importlib.util.spec_from_file_location('bootstrap', ROOT / 'scripts/bootstrap.py')
        bootstrap = importlib.util.module_from_spec(spec); spec.loader.exec_module(bootstrap)
        lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
        base = ROOT / 'build' / ('macos' if os.uname().sysname == 'Darwin' else 'linux') / 'test-data'
        with tempfile.TemporaryDirectory(dir=base) as directory:
            native = Path(directory) / 'native'; native.mkdir()
            notice = native / 'share/licenses/libvterm/LICENSE'; notice.parent.mkdir(parents=True)
            notice.write_text('owned synthetic MIT notice')
            archive = native / 'lib/libvterm.a'; archive.parent.mkdir(); archive.write_bytes(b'static SDK archive')
            contract = {'version': lock['versions']['libvterm'], 'source': lock['artifacts']['libvterm']['sha256']}
            bootstrap.record_dependency(native, 'libvterm', contract, ['share/licenses/libvterm/LICENSE'])
            for host, relative in (('macos', 'Contents/Resources/third-party'), ('linux', 'share/third-party')):
                destination = Path(directory) / host
                package.stage_libvterm_notice(native, destination, host)
                self.assertEqual((destination / relative / 'libvterm-LICENSE.txt').read_bytes(), notice.read_bytes())
                self.assertEqual(list(destination.rglob('libvterm.a')), [])
                self.assertEqual(list(destination.rglob('vterm.h')), [])
            notice.write_text('changed notice')
            with self.assertRaisesRegex(RuntimeError, 'notice'):
                package.stage_libvterm_notice(native, Path(directory) / 'rejected', 'linux')

    def test_rust_notices_require_locked_sources_and_complete_licenses(self):
        import hashlib
        import io
        import tarfile
        import tempfile
        spec = importlib.util.spec_from_file_location('rust_notices', ROOT / 'scripts/rust_notices.py')
        notices = importlib.util.module_from_spec(spec); spec.loader.exec_module(notices)
        base = ROOT / 'build' / ('macos' if os.uname().sysname == 'Darwin' else 'linux') / 'test-data'
        with tempfile.TemporaryDirectory(dir=base) as directory:
            home = Path(directory)
            crate = home / 'registry/cache/owned/fixture-1.0.0.crate'; crate.parent.mkdir(parents=True)
            with tarfile.open(crate, 'w:gz') as archive:
                entry = tarfile.TarInfo('fixture-1.0.0/LICENSE'); data = b'owned MIT license'
                entry.size = len(data); archive.addfile(entry, io.BytesIO(data))
            digest = hashlib.sha256(crate.read_bytes()).hexdigest()
            lock = ('[[package]]\nname = "fixture"\nversion = "1.0.0"\n'
                    'source = "registry+https://github.com/rust-lang/crates.io-index"\n'
                    'checksum = "' + digest + '"\n').encode()
            metadata = {'packages': [{'name': 'fixture', 'version': '1.0.0', 'license': 'MIT',
                'source': 'registry+https://github.com/rust-lang/crates.io-index',
                'manifest_path': str(home / 'registry/src/owned/fixture-1.0.0/Cargo.toml'), 'targets': []}],
                'resolve': {'nodes': []}}
            records = notices.collect(metadata, lock, home, {}, home)
            self.assertEqual(records[0]['checksum'], digest)
            self.assertEqual(records[0]['notices']['LICENSE'], data)
            for license in ('0BSD OR MIT OR Apache-2.0', 'Unlicense', 'MIT OR Zlib OR Apache-2.0'):
                with self.subTest(license=license):
                    metadata['packages'][0]['license'] = license
                    records = notices.collect(metadata, lock, home, {}, home)
                    self.assertEqual(records[0]['license'], license)
                    self.assertEqual(records[0]['checksum'], digest)
                    self.assertEqual(records[0]['notices']['LICENSE'], data)
            metadata['packages'][0]['license'] = 'unreviewed license'
            with self.assertRaisesRegex(ValueError, 'license'):
                notices.collect(metadata, lock, home, {}, home)
            metadata['packages'][0]['license'] = 'MIT'
            crate.write_bytes(b'changed crate')
            with self.assertRaisesRegex(ValueError, 'checksum'):
                notices.collect(metadata, lock, home, {}, home)

    def test_verifier_preserves_environment(self):
        before = dict(os.environ)
        verify.cache_dir('macos')
        self.assertEqual(before, dict(os.environ))
