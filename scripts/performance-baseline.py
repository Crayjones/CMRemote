#!/usr/bin/env python3
"""Freeze and verify a contained, unoptimized macOS performance client for replay."""
import argparse
import hashlib
import json
import plistlib
from pathlib import Path
import shutil
import subprocess
import sys
import uuid
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def files(bundle):
    result = {}
    for path in sorted(bundle.rglob('*')):
        if path.is_symlink():
            try: path.resolve(strict=True).relative_to(bundle.resolve())
            except (ValueError, FileNotFoundError): raise RuntimeError('Baseline symlink escapes bundle') from None
            result[str(path.relative_to(bundle))] = {'link': str(path.readlink())}
        elif path.is_file(): result[str(path.relative_to(bundle))] = {'sha256': digest(path)}
    return result


def verify(directory, build):
    directory = directory.resolve()
    try: directory.relative_to((build / 'performance').resolve())
    except ValueError: raise RuntimeError('Baseline must stay inside the platform performance directory') from None
    if not directory.name.startswith('baseline-'): raise RuntimeError('Invalid baseline directory')
    manifest = json.loads((directory / 'manifest.json').read_text())
    bundle = directory / 'PerformanceBaseline.app'
    if type(manifest.get('schema')) is not int or manifest.get('schema') != 1 or \
            manifest.get('policy') != 'legacy-unoptimized' or manifest.get('contained_runtime') is not True or \
            files(bundle) != manifest.get('files'):
        raise RuntimeError('Baseline executable or dependencies changed')
    if not (bundle / 'Contents/MacOS/performance_client').is_file(): raise RuntimeError('Baseline executable missing')
    return bundle


def runtime_paths(output):
    lines = output.splitlines()
    return [lines[i + 2].strip().split(' (', 1)[0].removeprefix('path ')
            for i, line in enumerate(lines[:-2]) if line.strip() == 'cmd LC_RPATH']


def audit_loads(output):
    for line in output.splitlines():
        if not line.startswith('\t'): continue  # Universal binaries have one header per architecture.
        dependency = line.strip().split(' (', 1)[0]
        if not dependency.startswith(('@rpath/', '@loader_path/', '@executable_path/', '/usr/lib/', '/System/Library/')):
            raise RuntimeError('Baseline has an uncontained Mach-O dependency')


def freeze(env):
    if sys.platform != 'darwin': raise RuntimeError('Baseline freezing currently requires macOS')
    worker = (ROOT / 'app/RdpWorker.cpp').read_text()
    if any(anchor not in worker for anchor in ('WaitForMultipleObjects(count, events, FALSE, 16)',
            'commands.swap(commands_)', 'FreeRDP_SupportGraphicsPipeline, FALSE',
            'copyFramebuffer(g->primary_buffer, g->width, g->height, g->stride)')):
        raise RuntimeError('Unoptimized worker source no longer matches the baseline policy')
    build = Path(env['RM_BUILD_DIR'])
    source = build / 'app/performance_client'
    if not source.is_file(): raise RuntimeError('Build the unoptimized performance client first')
    directory = build / 'performance' / ('baseline-' + uuid.uuid4().hex)
    bundle = directory / 'PerformanceBaseline.app'
    executable = bundle / 'Contents/MacOS/performance_client'
    executable.parent.mkdir(parents=True)
    shutil.copy2(source, executable)
    (bundle / 'Contents/Info.plist').write_bytes(plistlib.dumps({
        'CFBundleExecutable': 'performance_client', 'CFBundleIdentifier': 'dev.remotemanager.performance-baseline',
        'CFBundleName': 'Owned Performance Baseline', 'CFBundlePackageType': 'APPL', 'NSHighResolutionCapable': True}))
    subprocess.run([str(Path(env['RM_QT_PREFIX']) / 'bin/macdeployqt'), str(bundle), '-always-overwrite',
        '-libpath=' + str(Path(env['RM_NATIVE_PREFIX']) / 'lib'), '-codesign=-'], env=env, check=True)
    for path in bundle.rglob('*'):
        if not path.is_file() or path.is_symlink(): continue
        result = subprocess.run(['otool', '-L', str(path)], capture_output=True, text=True)
        if result.returncode != 0: continue
        audit_loads(result.stdout)
        paths = subprocess.run(['otool', '-l', str(path)], capture_output=True, text=True, check=True)
        for runtime in set(runtime_paths(paths.stdout)):
            if runtime.startswith(str(ROOT) + '/'):
                subprocess.run(['install_name_tool', '-delete_rpath', runtime, str(path)], check=True)
            elif not runtime.startswith(('@loader_path/', '@executable_path/', '/usr/lib/', '/System/Library/')):
                raise RuntimeError('Baseline has an uncontained runtime search path')
    subprocess.run(['codesign', '--force', '--deep', '--sign', '-', str(bundle)], check=True, capture_output=True)
    manifest = {'schema': 1, 'policy': 'legacy-unoptimized', 'platform': 'macos', 'contained_runtime': True,
        'versions': json.loads((ROOT / 'dependencies.lock.json').read_text())['versions'],
        'sources': {str(path.relative_to(ROOT)): digest(path) for path in
                    [ROOT / 'CMakeLists.txt', ROOT / 'tests/performance_client.cpp', ROOT / 'tests/PerformanceMarker.h',
                     ROOT / 'tests/NativeTestWindow.mm', ROOT / 'tests/RhiPixelOracle.h',
                     ROOT / 'app/RdpWorker.cpp', ROOT / 'app/RdpWorker.h', ROOT / 'app/Views.cpp', ROOT / 'app/Views.h',
                     ROOT / 'app/PerformanceTrace.cpp', ROOT / 'app/PerformanceTrace.h']},
        'files': files(bundle)}
    (directory / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    verify(directory, build)
    return directory


def main():
    from env import environment
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify', type=Path)
    args = parser.parse_args()
    env = environment()
    print(verify(args.verify, Path(env['RM_BUILD_DIR'])) if args.verify else freeze(env))


if __name__ == '__main__': main()
