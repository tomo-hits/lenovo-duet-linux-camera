#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Assemble an update archive from checksummed source, signed APKs and full source.

This copies release inputs only. It does not sign, install, mount or boot them.
APK signatures must be verified by the native packager and again at installation.
"""
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tarfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
AUTHOR_PACKAGE_MANIFEST = 'developer/docs/PUBLIC_PACKAGES.json'
spec = importlib.util.spec_from_file_location('bundle_update_policy', ROOT/'developer/scripts/apply-update.py')
update = importlib.util.module_from_spec(spec)
spec.loader.exec_module(update)


def require(ok, message):
    if not ok:
        raise ValueError(message)


def safe_relative(name):
    path = PurePosixPath(name)
    require(name and not path.is_absolute() and '..' not in path.parts and '\\' not in name,
            'Unsafe bundle path: '+name)
    require(str(path) == name and '\n' not in name and '\r' not in name, 'Non-canonical bundle path')
    return name


def regular(path, base=None):
    if base is not None:
        relative = path.relative_to(base)
        for parent in relative.parents:
            require(not (base/parent).is_symlink(), 'Input parent is a symlink: '+str(base/parent))
    require(path.is_file() and not path.is_symlink(), 'Expected regular input: '+str(path))
    return path.read_bytes()


def verify_tuning(entries, manifest, payloads, full_bytes):
    """Reject stale tuning even when each input has a valid outer checksum.

    The native packager separately verifies signatures and extracts real APKs.
    This check connects its payload hashes to this source and full-source tar.
    """
    names = ('ov02a10.yaml', 'ov8856.yaml', 'uncalibrated.yaml')
    ipa = [name for name, info in manifest.items() if info['name'] == 'libcamera-ipa']
    require(len(ipa) == 1, 'Expected one libcamera IPA package')
    expected = {}
    for name in names:
        source_name = 'developer/tuning/' + name
        require(source_name in entries, 'Missing source tuning: ' + name)
        data = entries[source_name][0]
        payload_name = 'usr/share/libcamera/ipa/softisp/' + name
        require(payloads.get(ipa[0], {}).get(payload_name) == hashlib.sha256(data).hexdigest(),
                'APK payload tuning differs from source: ' + name)
        expected['mt8183-camera-complete-sources/release-tuning/' + name] = data
    seen = set()
    matched = set()
    with tarfile.open(fileobj=io.BytesIO(full_bytes), mode='r:*') as archive:
        for member in archive:
            safe_relative(member.name)
            require(member.name not in seen, 'Duplicate complete-source member')
            seen.add(member.name)
            if member.name not in expected:
                continue
            data = expected[member.name]
            require(member.isfile() and member.size == len(data),
                    'Invalid complete-source tuning member: ' + member.name)
            require(archive.extractfile(member).read() == data,
                    'Complete-source tuning differs from source: ' + member.name)
            matched.add(member.name)
    require(matched == set(expected), 'Missing complete-source tuning')


def assemble(root, packages, full_source, output, expected_key):
    entries = {}

    def add(name, data, mode=0o644):
        safe_relative(name)
        require(name not in entries, 'Duplicate bundle path: '+name)
        entries[name] = (data, mode)

    checksums = regular(root/'SHA256SUMS', root)
    for line in checksums.decode().splitlines():
        digest, name = line.split('  ', 1)
        safe_relative(name)
        require(name not in ('SHA256SUMS', 'BUNDLE.json', 'BUNDLE_SHA256SUMS') and
                not name.startswith(('packages/', 'sources/', '.git/')), 'Invalid source manifest entry')
        source = root/name
        data = regular(source, root)
        require(hashlib.sha256(data).hexdigest() == digest, 'Source digest mismatch: '+name)
        add(name, data, 0o755 if source.stat().st_mode & 0o111 else 0o644)
    add('SHA256SUMS', checksums)
    for name in ('install.sh', 'restore.sh', 'lib/entrypoint.py', 'developer/scripts/apply-update.py'):
        require(name in entries, 'Missing installation entry point: '+name)

    manifest_bytes = regular(packages/'packages.json', packages)
    manifest = json.loads(manifest_bytes)
    require(len(manifest) == 11, 'Expected the complete 11-package set')
    for filename, info in manifest.items():
        require(filename == info['name']+'-'+info['version']+'.apk' and '/' not in filename and
                info['arch'] == 'aarch64', 'Invalid package identity')
        data = regular(packages/'repo/aarch64'/filename, packages)
        require(hashlib.sha256(data).hexdigest() == info['sha256'], 'Package digest mismatch: '+filename)
        add('packages/repo/aarch64/'+filename, data)
    for name, version in update.PINS.items():
        require(name+'-'+version+'.apk' in manifest, 'Missing pinned runtime package: '+name)
    # The author-key candidate must retain the recorded signed bytes. Matching
    # this manifest is not hardware acceptance of the internal-mode candidate.
    if expected_key == update.KEY_HASH:
        recorded = json.loads(regular(root/AUTHOR_PACKAGE_MANIFEST, root))
        require(manifest == recorded, 'Author-key release differs from the recorded package set')
    keys = list((packages/'keys').glob('*.pub'))
    require(len(keys) == 1, 'Expected one public signing key')
    key_bytes = regular(keys[0], packages)
    der = subprocess.run(['openssl', 'pkey', '-pubin', '-outform', 'DER'], input=key_bytes,
                         check=True, capture_output=True).stdout
    require(hashlib.sha256(der).hexdigest() == expected_key, 'Signing public key mismatch')
    add('packages/keys/'+keys[0].name, key_bytes)
    add('packages/packages.json', manifest_bytes)
    for name in ('repo/aarch64/packages.adb', 'payload-hashes.json'):
        add('packages/'+name, regular(packages/name, packages))
    if (packages/'correspondence.json').exists():
        add('packages/correspondence.json', regular(packages/'correspondence.json', packages))
    supplemental = []
    if (packages/'official-zstd').exists():
        apk, index = update.official_zstd_inputs(packages)
        for path in (apk, index):
            add('packages/official-zstd/'+path.name, regular(path, packages))
        supplemental.append('zstd='+update.OFFICIAL_ZSTD['version'])

    complete = json.loads(regular(root/'developer/docs/COMPLETE_SOURCES.json', root))
    full_bytes = regular(full_source)
    require(len(full_bytes) == complete['bytes'] and hashlib.sha256(full_bytes).hexdigest() == complete['sha256'],
            'Complete corresponding source archive mismatch')
    verify_tuning(entries, manifest, json.loads(entries['packages/payload-hashes.json'][0]), full_bytes)
    add('sources/'+safe_relative(complete['archive']), full_bytes)
    record = {
        'layout': 'User entry points at root; source kit and build/test records under developer/',
        'install': ('sudo sh install.sh' if expected_key == update.KEY_HASH else
                    'sudo sh install.sh --expected-key-sha256 '+expected_key),
        'restore': 'sudo sh restore.sh',
        'signing_key_spki_sha256': expected_key,
        'author_recorded_apks_byte_identical': expected_key == update.KEY_HASH,
        'accepted_package_manifest': AUTHOR_PACKAGE_MANIFEST if expected_key == update.KEY_HASH else None,
        'hardware_acceptance': False,
        'optional_official_packages': supplemental,
        'source_manifest_sha256': hashlib.sha256(checksums).hexdigest(),
        'complete_sources_sha256': complete['sha256'],
        'source_tuning_matches_complete_source_and_payload_manifest': True,
        'validation_scope': 'Archive hashes, public-key identity and source/full-source/payload-manifest tuning correspondence; no APK signature verification or new hardware acceptance.',
        'historical_records': 'Source-kit paths in historical JSON records are relative to developer/.',
    }
    add('BUNDLE.json', (json.dumps(record, indent=2)+'\n').encode())
    sums = ''.join(hashlib.sha256(data).hexdigest()+'  '+name+'\n'
                   for name, (data, mode) in sorted(entries.items()))
    add('BUNDLE_SHA256SUMS', sums.encode())
    # Exclusive creation preserves all previous candidates, including symlinks.
    with output.open('xb') as raw:
        with tarfile.open(fileobj=raw, mode='w', format=tarfile.PAX_FORMAT) as archive:
            for name, (data, mode) in sorted(entries.items()):
                item = tarfile.TarInfo('lenovo-duet-linux-camera/'+name)
                item.size, item.mode, item.mtime = len(data), mode, 0
                item.uid = item.gid = 0
                item.uname = item.gname = ''
                archive.addfile(item, io.BytesIO(data))
    with output.open('rb') as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest() if hasattr(hashlib, 'file_digest') else hashlib.sha256(source.read()).hexdigest()
    return {'archive': output.name, 'bytes': output.stat().st_size,
            'sha256': digest, 'files': len(entries), **record}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--packages', type=Path, required=True)
    parser.add_argument('--complete-sources', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expected-key-sha256', type=update.key_fingerprint, default=update.KEY_HASH)
    args = parser.parse_args()
    print(json.dumps(assemble(ROOT, args.packages, args.complete_sources, args.output,
                              args.expected_key_sha256), indent=2))


if __name__ == '__main__':
    main()
