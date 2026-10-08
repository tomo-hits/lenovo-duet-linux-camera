#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Duet camera project contributors
"""Reissue reviewed camera helpers/notices without rebuilding accepted ELF files.

Requires apk-tools 3.0.8, an extracted accepted v4 repository and a local key.
Never installs packages, executes package scripts or exports private keys.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--accepted-repository', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sign-key', type=Path, required=True)
    parser.add_argument('--public-key', type=Path, required=True)
    args = parser.parse_args()
    kit = Path(__file__).resolve().parent.parent
    baseline = json.loads((kit / 'docs/ACCEPTED_PACKAGES.json').read_text())
    source = args.accepted_repository.resolve()
    require(args.sign_key.is_file() and args.public_key.is_file(), 'Missing local signing key')
    require(len(baseline) == 11, 'Expected eleven accepted packages')
    for filename, info in baseline.items():
        require(digest(source / 'repo/aarch64' / filename) == info['sha256'],
                'Accepted package hash mismatch: ' + filename)
    fingerprint = subprocess.check_output([
        'openssl', 'pkey', '-pubin', '-in', str(args.public_key), '-outform', 'DER'
    ])
    require(hashlib.sha256(fingerprint).hexdigest() ==
            '94d05c05d71e63aa74b0a2f11a4f4e3d8138e701daf5fe4f95e980b8fef73cd9',
            'Wrong accepted signing public key')
    out = args.output.absolute()
    require(not out.exists() and not out.is_symlink(), 'Output must be a new directory')
    out.mkdir()  # Refuse an existing output, including a symlink.
    keys = out / 'keys'
    keys.mkdir()
    shutil.copy2(args.public_key, keys / args.public_key.name)
    repo = out / 'repo/aarch64'
    repo.mkdir(parents=True)
    stages = out / 'stages'
    stages.mkdir()
    changes = {
        'duet-camera-modules-0.2.3-r0.apk': '0.2.3-r1',
        'duet-camera-config-0.1.2-r0.apk': '0.1.3-r0',
        'duet-camera-config-0.1.2-r1.apk': '0.1.3-r1',
        'duet-camera-0.2.4-r0.apk': '0.2.5-r0',
        'duet-camera-0.2.4-r1.apk': '0.2.5-r1',
    }
    original_payloads = json.loads((source / 'payload-hashes.json').read_text())
    manifest, payloads, correspondence = {}, {}, {}
    for filename, old_info in baseline.items():
        old_apk = source / 'repo/aarch64' / filename
        subprocess.run(['apk', 'verify', '--keys-dir', str(keys), str(old_apk)], check=True)
        if filename not in changes:
            shutil.copy2(old_apk, repo / filename)
            manifest[filename] = old_info
            payloads[filename] = original_payloads[filename]
            continue
        info = {k: v for k, v in old_info.items() if k != 'sha256'}
        info['version'] = changes[filename]
        info['depends'] = info['depends'].replace(
            'duet-camera-modules=0.2.3-r0', 'duet-camera-modules=0.2.3-r1'
        ).replace('duet-camera-config=0.1.2-r', 'duet-camera-config=0.1.3-r')
        new_name = info['name'] + '-' + info['version'] + '.apk'
        stage = stages / new_name.removesuffix('.apk')
        stage.mkdir()
        subprocess.run(['apk', 'extract', '--keys-dir', str(keys), '--no-chown',
                        '--destination', str(stage), str(old_apk)], check=True)
        for relative, expected in original_payloads[filename].items():
            require(digest(stage / relative) == expected, 'Extracted payload mismatch: ' + relative)
        if info['name'] == 'duet-camera-modules':
            for name in ('integrate-camera-dtb.py', 'legacy_bindings.py'):
                shutil.copy2(kit / 'packaging/postmarketos' / name,
                             stage / 'usr/lib/duet-camera' / name)
        license_dir = stage / 'usr/share/licenses' / info['name']
        shutil.copy2(kit / 'LICENSES/PROJECT-MIT.txt', license_dir / 'PROJECT-MIT.txt')
        # Check the exact changed-file allowance; every accepted ELF and config stays intact.
        updated = {str(p.relative_to(stage)): digest(p) for p in stage.rglob('*')
                   if p.is_file() and not p.is_symlink()}
        changed = {p for p in set(updated) | set(original_payloads[filename])
                   if updated.get(p) != original_payloads[filename].get(p)}
        allowed = {'usr/share/licenses/' + info['name'] + '/PROJECT-MIT.txt'}
        if info['name'] == 'duet-camera-modules':
            allowed |= {'usr/lib/duet-camera/' + n for n in
                        ('integrate-camera-dtb.py', 'legacy_bindings.py')}
        require(changed == allowed, 'Unexpected payload changes')
        apk = repo / new_name
        command = ['apk', 'mkpkg', '--files', str(stage), '--output', str(apk),
                   '--sign-key', str(args.sign_key)]
        for key, value in info.items():
            command.extend(['--info', key + ':' + value])
        subprocess.run(command, check=True)
        subprocess.run(['apk', 'verify', '--keys-dir', str(keys), str(apk)], check=True)
        manifest[new_name] = dict(info, sha256=digest(apk))
        payloads[new_name] = updated
        correspondence[new_name] = {'accepted_package': filename,
                                    'accepted_sha256': old_info['sha256'],
                                    'changed_payload_files': sorted(changed),
                                    'elf_payload_unchanged': True}
    subprocess.run(['apk', 'mkndx', '--sign-key', str(args.sign_key),
                    '--output', str(repo / 'packages.adb'),
                    *map(str, sorted(repo.glob('*.apk')))], check=True)
    subprocess.run(['apk', 'verify', '--keys-dir', str(keys),
                    str(repo / 'packages.adb')], check=True)
    for name, data in [('packages.json', manifest), ('payload-hashes.json', payloads),
                       ('correspondence.json', correspondence)]:
        (out / name).write_text(json.dumps(data, indent=2) + '\n')
    print(json.dumps({'signed_packages': len(manifest), 'reissued_packages': len(changes),
                      'unchanged_packages': len(baseline) - len(changes),
                      'accepted_elf_payload_unchanged': True}))


if __name__ == '__main__':
    main()
