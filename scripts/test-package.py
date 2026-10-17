#!/usr/bin/env python3
"""Check a relocated Linux package with WinPR crypto and Mesa software rendering."""
import argparse
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
RHI_CASES = ('rhiPresentsFrameInsideShownMainWindow', 'rhiPresentsFrameAndGrabMatches',
             'rhiRetainsFrameAfterResourceRelease', 'rhiNullFrameClearsPresentation')
REQUIRED = ('RemoteManager', 'bin/RemoteManager', 'bin/qt.conf', 'lib/libcrypto.so.3',
            'lib/libssl.so.3', 'lib/libwinpr3.so.3', 'lib/ossl-modules/legacy.so',
            'plugins/platforms/libqxcb.so', 'plugins/xcbglintegrations/libqxcb-glx-integration.so',
            'plugins/xcbglintegrations/libqxcb-egl-integration.so')


def relocate(artifact, destination):
    if artifact.is_dir():
        for path in artifact.rglob('*'):
            if path.is_symlink() and not path.resolve().is_relative_to(artifact.resolve()):
                raise RuntimeError('Package symlink escapes artifact')
        shutil.copytree(artifact, destination / artifact.name)
    else:
        with tarfile.open(artifact, 'r:gz') as archive:
            members = archive.getmembers()
            for member in members:
                path = Path(member.name)
                if path.is_absolute() or '..' in path.parts or not (member.isfile() or member.isdir()):
                    raise RuntimeError('Unsafe Linux package archive member')
            archive.extractall(destination)
    roots = list(destination.iterdir())
    if len(roots) != 1 or not roots[0].is_dir(): raise RuntimeError('Expected one package directory')
    bundle = roots[0]
    for name in REQUIRED:
        if not (bundle / name).is_file(): raise RuntimeError('Missing required package runtime: ' + name)
    return bundle


def runtime_environment(temp):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('LD_', 'QT_', 'OPENSSL_', 'RM_'))}
    env.update(PATH='/usr/bin:/bin', LIBGL_ALWAYS_SOFTWARE='1',
               TMPDIR=str(temp), TMP=str(temp), TEMP=str(temp),
               XDG_DATA_HOME=str(temp / 'data'), XDG_CONFIG_HOME=str(temp / 'config'),
               XDG_CACHE_HOME=str(temp / 'cache'), PYTHONDONTWRITEBYTECODE='1')
    return env


def checked_run(command, env):
    result = subprocess.run([str(arg) for arg in command], env=env, capture_output=True, text=True, timeout=180)
    report = result.stdout + result.stderr
    if result.returncode or any(text in report.lower() for text in
            ('qrhiwidget: no qrhi', 'failed to create qrhi', 'unreleased resources',
             'failed to initialize digest md4')):
        raise RuntimeError('Package runtime check failed:\n' + report)
    return report


def audit_elf(bundle, env):
    for directory in ('bin', 'lib', 'plugins'):
        for path in (bundle / directory).rglob('*'):
            if not path.is_file(): continue
            with path.open('rb') as source:
                if source.read(4) != b'\x7fELF': continue
            rpaths = checked_run(['patchelf', '--print-rpath', path], env).strip().split(':')
            for rpath in rpaths:
                if not rpath.startswith('$ORIGIN') or not (
                        path.parent / rpath.removeprefix('$ORIGIN').lstrip('/')).resolve().is_relative_to(bundle):
                    raise RuntimeError('Uncontained ELF runtime path: ' + str(path))
            listing = checked_run(['ldd', path], env)
            if 'not found' in listing: raise RuntimeError('Unresolved packaged dependency:\n' + listing)
            for dependency in re.findall(r'=>\s+(/\S+)', listing):
                resolved = Path(dependency).resolve()
                if not resolved.is_relative_to(bundle) and not any(
                        resolved.is_relative_to(Path(system)) for system in ('/usr/lib', '/lib')):
                    raise RuntimeError('Package depends on an SDK or other external prefix: ' + dependency)


def launcher_wrapper(bundle):
    launcher = (bundle / 'RemoteManager').read_text()
    entry = 'exec "$DIR/bin/RemoteManager" "$@"'
    if launcher.count(entry) != 1: raise RuntimeError('Unrecognized package launcher')
    wrapper = bundle / 'runtime-check'
    wrapper.write_text(launcher.replace(entry, 'exec "$@"'))
    wrapper.chmod(0o755)
    return wrapper


def crypto_probe(bundle):
    if os.environ.get('OPENSSL_MODULES') != str(bundle / 'lib/ossl-modules'):
        raise RuntimeError('Launcher did not select the bundled OpenSSL modules')
    winpr = ctypes.CDLL(str(bundle / 'lib/libwinpr3.so.3'))
    winpr.winpr_InitializeSSL.argtypes = [ctypes.c_uint32]
    winpr.winpr_InitializeSSL.restype = ctypes.c_int
    if not winpr.winpr_InitializeSSL(0): raise RuntimeError('WinPR SSL initialization failed')
    winpr.winpr_md_type_from_string.argtypes = [ctypes.c_char_p]
    winpr.winpr_md_type_from_string.restype = ctypes.c_int
    winpr.winpr_Digest.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
    winpr.winpr_Digest.restype = ctypes.c_int
    for name, answer in [(b'MD4', 'a448017aaf21d8525fc10ae87aa6729d'),
                         (b'SHA256', 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')]:
        output = ctypes.create_string_buffer(len(answer) // 2)
        if not winpr.winpr_Digest(winpr.winpr_md_type_from_string(name), b'abc', 3, output, len(output)) or output.raw.hex() != answer:
            raise RuntimeError('WinPR digest known answer failed: ' + name.decode())
    crypto = ctypes.CDLL(str(bundle / 'lib/libcrypto.so.3'))
    crypto.OpenSSL_version.argtypes = [ctypes.c_int]; crypto.OpenSSL_version.restype = ctypes.c_char_p
    version = json.loads((ROOT / 'dependencies.lock.json').read_text())['versions']['openssl']
    if not crypto.OpenSSL_version(0).decode().startswith('OpenSSL ' + version + ' '):
        raise RuntimeError('Bundled OpenSSL version does not match the pin')
    crypto.OSSL_PROVIDER_available.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    crypto.OSSL_PROVIDER_available.restype = ctypes.c_int
    for name in (b'legacy', b'default'):
        if not crypto.OSSL_PROVIDER_available(None, name): raise RuntimeError('Missing OpenSSL provider')
    crypto.EVP_CIPHER_fetch.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    crypto.EVP_CIPHER_fetch.restype = ctypes.c_void_p
    crypto.EVP_CIPHER_free.argtypes = [ctypes.c_void_p]
    cipher = crypto.EVP_CIPHER_fetch(None, b'AES-256-GCM', None)
    if not cipher: raise RuntimeError('Modern AES-GCM unavailable after legacy load')
    crypto.EVP_CIPHER_free(cipher)
    crypto.BIO_new_mem_buf.argtypes = [ctypes.c_void_p, ctypes.c_int]; crypto.BIO_new_mem_buf.restype = ctypes.c_void_p
    crypto.BIO_free.argtypes = [ctypes.c_void_p]
    bio = crypto.BIO_new_mem_buf(b'owned synthetic data', 20)
    if not bio: raise RuntimeError('OpenSSL memory BIO allocation failed')
    crypto.BIO_free(bio)
    ssl = ctypes.CDLL(str(bundle / 'lib/libssl.so.3'))
    ssl.TLS_client_method.restype = ctypes.c_void_p
    ssl.SSL_CTX_new.argtypes = [ctypes.c_void_p]; ssl.SSL_CTX_new.restype = ctypes.c_void_p
    ssl.SSL_CTX_free.argtypes = [ctypes.c_void_p]
    context = ssl.SSL_CTX_new(ssl.TLS_client_method())
    if not context: raise RuntimeError('TLS context initialization failed')
    ssl.SSL_CTX_free(context)
    mappings = Path('/proc/self/maps').read_text()
    for name in ('libcrypto.so.3', 'libssl.so.3', 'libwinpr3.so.3', 'legacy.so'):
        paths = [Path(line.split()[-1]).resolve() for line in mappings.splitlines() if line.split()[-1].endswith('/' + name)]
        if not paths or any(not path.is_relative_to(bundle) for path in paths):
            raise RuntimeError('Crypto runtime was not loaded exclusively from the artifact: ' + name)
    print('Packaged WinPR MD4/SHA256, legacy/default providers, AES-GCM, memory BIO and TLS: passed')


def accepted_rhi_report(report):
    totals = re.search(r'^Totals: (\d+) passed, (\d+) failed, (\d+) skipped,', report, re.M)
    return bool(totals and int(totals[1]) == len(RHI_CASES) + 2 and int(totals[2]) == 0 and int(totals[3]) == 0
                and all('PASS' in line for line in report.splitlines() if any(case + '(' in line for case in RHI_CASES))
                and all(case + '(' in report for case in RHI_CASES)
                and 'Finished testing' in report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('artifact', type=Path)
    parser.add_argument('--rhi-probe', type=Path)
    parser.add_argument('--qt-test-library', type=Path)
    parser.add_argument('--crypto-probe', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if sys.platform != 'linux': parser.error('Run inside the Linux development container')
    if args.crypto_probe:
        crypto_probe(args.artifact.resolve()); return 0
    if not args.rhi_probe or not args.qt_test_library: parser.error('RHI probe and its matching Qt Test library are required')
    artifact = args.artifact.resolve()
    if not artifact.is_relative_to(ROOT / 'dist/linux') or not artifact.exists():
        parser.error('Artifact must exist under dist/linux')
    arch = 'arm64' if platform.machine() in ('arm64', 'aarch64') else 'amd64'
    build = ROOT / 'build/linux' / arch
    base = build / 'package-tests'; base.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('package_display', ROOT / 'scripts/test-native.py')
    native = importlib.util.module_from_spec(spec); spec.loader.exec_module(native)
    with tempfile.TemporaryDirectory(prefix='relocated-', dir=base) as directory:
        temp = Path(directory).resolve(); bundle = relocate(artifact, temp / 'unpacked')
        env = runtime_environment(temp)
        audit_elf(bundle, env)
        wrapper = launcher_wrapper(bundle)
        print(checked_run([wrapper, sys.executable, Path(__file__).resolve(), '--crypto-probe', bundle], env), end='')
        # Test-only executable/library never enter the shipping archive.
        probe = bundle / 'bin/qt_smoke'; shutil.copy2(args.rhi_probe, probe)
        test_library = bundle / 'lib/libQt6Test.so.6'; shutil.copy2(args.qt_test_library, test_library)
        checked_run(['patchelf', '--set-rpath', '$ORIGIN/../lib', probe], env)
        checked_run(['patchelf', '--set-rpath', '$ORIGIN', test_library], env)
        audit_elf(bundle, env)
        with native.native_display('xcb', env, build) as display:
            print(checked_run([bundle / 'RemoteManager', '--smoke'], display), end='')
            report = checked_run([wrapper, probe, *RHI_CASES], display)
            print(report, end='')
            if not accepted_rhi_report(report): raise RuntimeError('Package RHI checks failed or skipped')
    print('Relocated Linux package runtime checks: passed (software OpenGL; no remote connections)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
