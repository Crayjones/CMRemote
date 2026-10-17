#!/usr/bin/env python3
"""Check locked registry/vendored archives and retain reviewed license notices."""
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


def project_file(relative):
    path = ROOT / relative
    if (Path(relative).is_absolute() or '..' in Path(relative).parts
            or path.is_symlink() or path.resolve() != ROOT.resolve() / relative
            or not path.is_file()):
        raise ValueError('Unsafe or missing vendored Rust file: ' + str(relative))
    return path


def vendored_record(package, artifacts, vendored):
    name, version = package['name'], package['version']
    contract = vendored.get(name)
    if (name != 'vnc-rs' or version != '0.6.0' or not contract
            or contract['version'] != version or contract['path'] != 'vendor/vnc-rs'
            or package['license'] != contract['license']
            or package['license'] not in REVIEWED_LICENSES):
        raise ValueError('Unreviewed vendored Rust dependency or license: ' + name)
    manifest = project_file(contract['path'] + '/Cargo.toml')
    if Path(package['manifest_path']) != manifest:
        raise ValueError('Unreviewed vendored Rust dependency path: ' + name)
    section = re.search(r'(?ms)^\[package\]\s*\n(.*?)(?=^\[|\Z)', manifest.read_text())
    fields = {key: json.loads(value) for key, value in re.findall(
        r'^(name|version|license) = ("[^"\n]*")$', section.group(1) if section else '', re.M)}
    if fields != {'name': name, 'version': version, 'license': package['license']}:
        raise ValueError('Vendored Rust manifest differs from reviewed contract: ' + name)
    artifact = artifacts[contract['artifact']]
    crate = project_file(contract['archive'])
    notices = archive_notices(crate, name, version, artifact['sha256'])
    if not {'LICENSE-MIT', 'LICENSE-APACHE'}.issubset(notices):
        raise ValueError('Missing vendored Rust license notices: ' + name)
    for relative, data in notices.items():
        if project_file(contract['path'] + '/' + relative).read_bytes() != data:
            raise ValueError('Vendored Rust notice differs from pinned archive: ' + name)
    for field in ('provenance', 'patch_notes'):
        relative = contract[field]
        data = project_file(contract['path'] + '/' + relative).read_bytes()
        if not data.strip() or len(data) > 1024 * 1024:
            raise ValueError('Invalid vendored Rust provenance: ' + name)
        notices[relative] = data
    return {'name': name, 'version': version, 'license': package['license'],
            'checksum': artifact['sha256'], 'source': contract['path'],
            'archive_url': artifact['url'], 'provenance': contract['provenance'],
            'notices': notices}


def audit_vendored(lock):
    """Audit retained baseline/attribution without running Cargo or fetching bytes."""
    return [vendored_record({'name': name, 'version': contract['version'],
                            'license': contract['license'],
                            'manifest_path': str(ROOT / contract['path'] / 'Cargo.toml')},
                           lock['artifacts'], lock['vendored_rust'])
            for name, contract in lock.get('vendored_rust', {}).items()]


def collect(metadata, lock_bytes, cargo_home, artifacts, cache, vendored=None):
    checksums = lock_checksums(lock_bytes)
    records = []
    for package in sorted(metadata['packages'], key=lambda p: (p['name'], p['version'])):
        if package['source'] is None:
            if (package['name'] == 'remotemanager-core'
                    and Path(package['manifest_path']) == ROOT / 'core/Cargo.toml'):
                continue
            records.append(vendored_record(package, artifacts, vendored or {}))
            continue
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
                      ROOT / '.dev' / env['RM_PLATFORM'] / 'downloads', lock.get('vendored_rust', {}))
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
        'scope': 'All locked registry and approved vendored packages, including build and non-native target dependencies; not a security audit. Vendored checksums identify the original archive, not patched source bytes.'
    }, indent=2) + '\n')
    return len(records)


if __name__ == '__main__':
    destination = Path(os.environ['RM_BUILD_DIR']) / 'deps/rust-notices'
    print('Retained notices for', stage(destination, os.environ), 'locked registry/vendored packages:', destination)
