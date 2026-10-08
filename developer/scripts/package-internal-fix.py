#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Duet camera project contributors
# Modified on 2026-10-06: reissue P1 explicit internal-eMMC opt-in and activation helper.
"""Package the P1 explicit internal-eMMC opt-in against the preserved external-USB candidate.

Requires apk-tools 3.0.8, an extracted preserved external-USB repository and a local key.
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
ACTIVATE_RELATIVE = 'usr/lib/duet-camera/duet-camera-activate.py'
PRE_INTERNAL_FIX_PACKAGES_SHA256 = '11787ac70acbcc37b38633b8fa9a8fc6f8fb9dc21fa80b60ad63e2b0e532d14b'
PRE_INTERNAL_FIX_PAYLOAD_SHA256 = '5e098f2d3874c04b7cb48fd5630c448440c37d76f1e456e04cf8c66b9af06dc8'
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
            'Payload package set differs from the preserved external-USB set')
    for entries in payloads.values():
        require(isinstance(entries, dict), 'Invalid payload file list')
        for relative, expected in entries.items():
            validate_relative_path(relative)
            require(isinstance(expected, str) and
                    re.fullmatch(r'[0-9a-f]{64}', expected), 'Invalid payload hash')
    require(P1_RELATIVE in payloads['duet-camera-modules-0.2.5-r0.apk'],
            'Missing canonical P1 module')
    require(ACTIVATE_RELATIVE in payloads['duet-camera-modules-0.2.5-r0.apk'],
            'Missing canonical activation helper')
    require(hashlib.sha256(data).hexdigest() == PRE_INTERNAL_FIX_PAYLOAD_SHA256,
            'Preserved r104 payload metadata hash mismatch')
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
    baseline_bytes = (kit / 'docs/PRE_INTERNAL_FIX_PACKAGES.json').read_bytes()
    require(hashlib.sha256(baseline_bytes).hexdigest() == PRE_INTERNAL_FIX_PACKAGES_SHA256,
            'Preserved r104 package metadata hash mismatch')
    record = json.loads(baseline_bytes, object_pairs_hook=unique_object)
    require(record['schema'] == 1 and
            record['payload_sha256'] == PRE_INTERNAL_FIX_PAYLOAD_SHA256,
            'Invalid preserved external-USB input record')
    baseline = record['packages']
    source = args.accepted_repository.resolve()
    require(digest(source / 'packages.json') == record['packages_sha256'],
            'Preserved external-USB package manifest hash mismatch')
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
    require(re.search(r'^parm:\s+external_usb:.*\(bool\)$', module_info, re.M),
            'P1 must expose the external_usb boolean opt-in')
    require(re.search(r'^parm:\s+internal_emmc:.*\(bool\)$', module_info, re.M),
            'P1 must expose the internal_emmc boolean opt-in')
    activate_bytes = (kit / 'packaging/postmarketos/duet-camera-activate.py').read_bytes()
    compile(activate_bytes, ACTIVATE_RELATIVE, 'exec')
    require(b'--external-usb' in activate_bytes, 'Activation helper lacks the external USB option')
    require(b'--internal-emmc' in activate_bytes,
            'Activation helper lacks the explicit internal eMMC option')
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
        'duet-camera-modules-0.2.5-r0.apk': '0.2.8-r0',
        'duet-camera-0.2.9-r0.apk': '0.2.12-r0',
        'duet-camera-0.2.9-r1.apk': '0.2.12-r1',
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
            'duet-camera-modules=0.2.5-r0', 'duet-camera-modules=0.2.8-r0'
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
            for relative, data in ((P1_RELATIVE, module_bytes),
                                   (ACTIVATE_RELATIVE, activate_bytes)):
                destination = stage_file(stage, relative)
                require(destination.stat().st_nlink == 1, 'Replacement destination must not be hard-linked')
                destination.write_bytes(data)
                allowed.add(relative)
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
                                    'p1_module_sha256': args.p1_sha256 if info['name'] == 'duet-camera-modules' else None,
                                    'activation_sha256': hashlib.sha256(activate_bytes).hexdigest() if info['name'] == 'duet-camera-modules' else None}
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
                      'only_p1_elf_changed': True, 'p1_sha256': args.p1_sha256,
                      'activation_sha256': hashlib.sha256(activate_bytes).hexdigest()}))


if __name__ == '__main__':
    main()
