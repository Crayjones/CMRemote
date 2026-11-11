#!/usr/bin/env python3
"""Check locked registry archives and retain reviewed license notices."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
REVIEWED_LICENSES = {
    'MIT', 'Apache-2.0', 'BSD-3-Clause', 'Zlib', 'ISC', 'bzip2-1.0.6',
    'MIT OR Apache-2.0', 'Apache-2.0 OR MIT', 'MIT/Apache-2.0', 'Unlicense OR MIT',
    'MIT OR Apache-2.0 OR BSD-1-Clause', 'MIT OR Apache-2.0 OR LGPL-2.1-or-later',
    '0BSD OR MIT OR Apache-2.0', 'Unlicense', 'MIT OR Zlib OR Apache-2.0',
    'Apache-2.0 AND ISC', '(MIT OR Apache-2.0) AND Unicode-3.0',
    'Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT',
}
UPSTREAM_NOTICES = {
    ('delegate', '0.13.5'): ['rust-notice-delegate-apache', 'rust-notice-delegate-mit'],
    ('pageant', '0.2.4'): ['rust-notice-pageant'],
    ('russh', '0.64.1'): ['rust-notice-russh'],
    ('russh-cryptovec', '0.62.0'): ['rust-notice-russh-cryptovec'],
    ('russh-util', '0.52.0'): ['rust-notice-russh-util'],
}


def lock_checksums(data):
    records = {}
    for block in data.decode().split('[[package]]')[1:]:
        fields = {key: json.loads(value) for key, value in
                  re.findall(r'^(name|version|source|checksum) = ("[^"\n]*")$', block, re.M)}
        if 'source' in fields:
            if fields['source'] != 'registry+https://github.com/rust-lang/crates.io-index':
                raise ValueError('Unreviewed Rust dependency source')
            records[(fields['name'], fields['version'])] = fields['checksum']
    return records


def archive_notices(crate, name, version, expected):
    if not expected or hashlib.sha256(crate.read_bytes()).hexdigest() != expected:
        raise ValueError('Rust dependency checksum mismatch: ' + name)
    notices = {}
    with tarfile.open(crate) as archive:
        prefix = name + '-' + version + '/'
        for member in archive.getmembers():
            if not member.name.startswith(prefix):
                raise ValueError('Invalid Rust archive root')
            relative = Path(member.name[len(prefix):])
            if relative.is_absolute() or '..' in relative.parts:
                raise ValueError('Unsafe Rust archive path')
            filename = relative.name.upper()
            if not (filename.startswith(('LICENSE', 'LICENCE', 'COPYING', 'NOTICE'))
                    or (name == 'r-efi' and filename == 'AUTHORS')):
                continue
            if not member.isfile() or member.size > 1024 * 1024:
                raise ValueError('Invalid Rust notice file')
            if str(relative) in notices:
                raise ValueError('Duplicate Rust notice file')
            notices[str(relative)] = archive.extractfile(member).read()
    return notices


def collect(metadata, lock_bytes, cargo_home, artifacts, cache):
    checksums = lock_checksums(lock_bytes)
    records = []
    for package in sorted(metadata['packages'], key=lambda p: (p['name'], p['version'])):
        if package['source'] is None:
            if (package['name'] == 'remotemanager-core'
                    and Path(package['manifest_path']) == ROOT / 'core/Cargo.toml'):
                continue
            raise ValueError('Unreviewed path Rust dependency: ' + package['name'])
        name, version = package['name'], package['version']
        if package['source'] != 'registry+https://github.com/rust-lang/crates.io-index':
            raise ValueError('Unreviewed Rust dependency source')
        if package['license'] not in REVIEWED_LICENSES:
            raise ValueError('Unreviewed Rust license: ' + name)
        expected = checksums.get((name, version))
        source = Path(package['manifest_path']).parent
        source_root = cargo_home / 'registry/src'
        if not source.resolve().is_relative_to(source_root.resolve()):
            raise ValueError('Rust dependency source escapes project CARGO_HOME')
        registry = source.relative_to(source_root).parts[0]
        crate = cargo_home / 'registry/cache' / registry / (name + '-' + version + '.crate')
        if not crate.resolve().is_relative_to(cargo_home.resolve()):
            raise ValueError('Rust dependency archive escapes project CARGO_HOME')
        notices = archive_notices(crate, name, version, expected)
        for key in UPSTREAM_NOTICES.get((name, version), []):
            artifact = artifacts[key]
            path = cache / artifact['file']
            if Path(artifact['file']).name != artifact['file'] or path.is_symlink():
                raise ValueError('Unsafe upstream notice path')
            data = path.read_bytes()
            if hashlib.sha256(data).hexdigest() != artifact['sha256']:
                raise ValueError('Upstream notice checksum mismatch')
            notices[artifact['file']] = data
        if not notices:
            raise ValueError('Missing Rust license notices: ' + name)
        records.append({'name': name, 'version': version, 'license': package['license'],
                        'checksum': expected, 'notices': notices})
    return records


def stage(destination, env):
    lock_bytes = (ROOT / 'Cargo.lock').read_bytes()
    metadata = json.loads(subprocess.check_output(
        ['cargo', 'metadata', '--locked', '--offline', '--format-version', '1'], cwd=ROOT, env=env))
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
    records = collect(metadata, lock_bytes, Path(env['CARGO_HOME']), lock['artifacts'],
                      ROOT / '.dev' / env['RM_PLATFORM'] / 'downloads')
    manifest = []
    for record in records:
        directory = destination / (record['name'] + '-' + record['version'])
        directory.mkdir(parents=True, exist_ok=True)
        hashes = {}
        for relative, data in record['notices'].items():
            target = directory / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            hashes[relative] = hashlib.sha256(data).hexdigest()
        manifest.append({**{k: v for k, v in record.items() if k != 'notices'}, 'notices': hashes})
    (destination / 'manifest.json').write_text(json.dumps({
        'schema': 1, 'cargo_lock_sha256': hashlib.sha256(lock_bytes).hexdigest(), 'packages': manifest,
        'scope': 'All locked registry packages, including build and non-native target dependencies; not a security audit.'
    }, indent=2) + '\n')
    return len(records)


if __name__ == '__main__':
    destination = Path(os.environ['RM_BUILD_DIR']) / 'deps/rust-notices'
    print('Retained notices for', stage(destination, os.environ), 'locked registry packages:', destination)
