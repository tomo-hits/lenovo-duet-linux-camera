#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Duet camera project contributors
# Modified on 2026-10-05: validate preserved metadata and contain P1 replacement.
"""Package the freshly compiled P1 idle-wait fix against the preserved v5 set.

Requires apk-tools 3.0.8, an extracted preserved v5 repository and a local key.
Never installs packages, executes package scripts or exports private keys.
"""
import argparse
import hashlib
import json
import shutil
import re
import struct
import subprocess
import stat
import tempfile
from pathlib import Path


P1_RELATIVE = 'lib/modules/6.18.28-mt81/extra/duet-camera/mt8183_p1.ko'
PRE_S1_PAYLOAD_SHA256 = '34efbd109b4b2d3ce72bc327fe3972e35cf23907fe84aa67e32e8e59b9eab88e'
PUBLIC_KEY_SHA256 = '94d05c05d71e63aa74b0a2f11a4f4e3d8138e701daf5fe4f95e980b8fef73cd9'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_relative_path(relative):
    require(isinstance(relative, str) and relative and
            '\\' not in relative and '\0' not in relative and
            all(part not in ('', '.', '..') for part in relative.split('/')),
            'Payload path must be a canonical relative path')


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate payload metadata key')
        result[key] = value
    return result


def load_payloads(path, baseline):
    # Validate the entire sidecar before creating any output or using its paths.
    data = path.read_bytes()
    payloads = json.loads(data, object_pairs_hook=unique_object)
    require(isinstance(payloads, dict) and set(payloads) == set(baseline),
            'Payload package set differs from the preserved v5 set')
    for entries in payloads.values():
        require(isinstance(entries, dict), 'Invalid payload file list')
        for relative, expected in entries.items():
            validate_relative_path(relative)
            require(isinstance(expected, str) and
                    re.fullmatch(r'[0-9a-f]{64}', expected), 'Invalid payload hash')
    require(P1_RELATIVE in payloads['duet-camera-modules-0.2.3-r1.apk'],
            'Missing canonical P1 module')
    require(hashlib.sha256(data).hexdigest() == PRE_S1_PAYLOAD_SHA256,
            'Preserved v5 payload metadata hash mismatch')
    return payloads


def stage_file(stage, relative):
    validate_relative_path(relative)
    require(stat.S_ISDIR(stage.lstat().st_mode), 'Stage must be a real directory')
    root = stage.resolve(strict=True)
    candidate = stage
    parts = relative.split('/')
    for index, part in enumerate(parts):
        candidate = candidate / part
        mode = candidate.lstat().st_mode
        require(stat.S_ISREG(mode) if index == len(parts) - 1 else stat.S_ISDIR(mode),
                'Payload must use real directories and a regular file')
    require(candidate.resolve(strict=True).is_relative_to(root),
            'Payload path escapes the stage')
    return candidate


def payload_hashes(stage):
    return {str(p.relative_to(stage)): digest(stage_file(stage, str(p.relative_to(stage))))
            for p in stage.rglob('*') if not p.is_symlink() and p.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--accepted-repository', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sign-key', type=Path, required=True)
    parser.add_argument('--public-key', type=Path, required=True)
    parser.add_argument('--p1-module', type=Path, required=True)
    parser.add_argument('--p1-sha256', required=True)
    args = parser.parse_args()
    kit = Path(__file__).resolve().parent.parent
    baseline = json.loads((kit / 'docs/PRE_S1_PACKAGES.json').read_text())
    source = args.accepted_repository.resolve()
    original_payloads = load_payloads(source / 'payload-hashes.json', baseline)
    # Hash, inspect and package the same snapshot, even if the build path changes.
    module_bytes = args.p1_module.read_bytes()
    require(hashlib.sha256(module_bytes).hexdigest() == args.p1_sha256,
            'P1 build hash mismatch')
    require(len(module_bytes) >= 20 and module_bytes[:7] == b'\x7fELF\x02\x01\x01' and struct.unpack_from('<HH', module_bytes, 16) == (1, 183), 'P1 must be an arm64 little-endian relocatable ELF64 module')
    with tempfile.TemporaryDirectory(prefix='duet-p1-input-') as folder:
        snapshot = Path(folder) / 'mt8183_p1.ko'
        snapshot.write_bytes(module_bytes)
        module_info = subprocess.check_output(['/sbin/modinfo', str(snapshot)], text=True)
    require(re.search(r'^vermagic:\s+6\.18\.28-mt81(?:\s|$)', module_info, re.M) and re.search(r'^name:\s+mt8183_p1$', module_info, re.M), 'Wrong P1 ABI/name')
    require(args.sign_key.is_file() and args.public_key.is_file(), 'Missing local signing key')
    require(len(baseline) == 11, 'Expected eleven accepted packages')
    for filename, info in baseline.items():
        require(digest(source / 'repo/aarch64' / filename) == info['sha256'],
                'Accepted package hash mismatch: ' + filename)
    fingerprint = subprocess.check_output([
        'openssl', 'pkey', '-pubin', '-in', str(args.public_key), '-outform', 'DER'
    ])
    require(hashlib.sha256(fingerprint).hexdigest() == PUBLIC_KEY_SHA256,
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
        'duet-camera-modules-0.2.3-r1.apk': '0.2.4-r0',
        'duet-camera-0.2.5-r0.apk': '0.2.6-r0',
        'duet-camera-0.2.5-r1.apk': '0.2.6-r1',
    }
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
            'duet-camera-modules=0.2.3-r1', 'duet-camera-modules=0.2.4-r0'
        )
        new_name = info['name'] + '-' + info['version'] + '.apk'
        stage = stages / new_name.removesuffix('.apk')
        stage.mkdir()
        subprocess.run(['apk', 'extract', '--keys-dir', str(keys), '--no-chown',
                        '--destination', str(stage), str(old_apk)], check=True)
        require(payload_hashes(stage) == original_payloads[filename],
                'Extracted payload differs from the preserved file list')
        allowed = set()
        if info['name'] == 'duet-camera-modules':
            destination = stage_file(stage, P1_RELATIVE)
            require(destination.stat().st_nlink == 1, 'P1 destination must not be hard-linked')
            destination.write_bytes(module_bytes)
            allowed.add(P1_RELATIVE)
        updated = payload_hashes(stage)
        changed = {p for p in set(updated) | set(original_payloads[filename])
                   if updated.get(p) != original_payloads[filename].get(p)}
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
                                    'elf_payload_unchanged': info['name'] != 'duet-camera-modules',
                                    'p1_module_sha256': args.p1_sha256 if info['name'] == 'duet-camera-modules' else None}
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
                      'only_p1_elf_changed': True, 'p1_sha256': args.p1_sha256}))


if __name__ == '__main__':
    main()
