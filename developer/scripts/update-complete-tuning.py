#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reissue the verified complete source with the rear Gamma 2.4 tuning only.

This is source archival, not a native build, APK signing or hardware test.
No archive member is extracted or executed. Previous inputs are never changed.
"""
import argparse
import copy
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import tarfile

ROOT = Path(__file__).resolve().parents[2]
PREFIX = 'mt8183-camera-complete-sources/'
OLD_SHA = '540367cfcd9b33c8a958f7a7f7aa8185d7c71a2a3ba6be265f8d1f7e36263ae4'
OLD_BYTES = 158776508
OLD_TUNING_SHA = 'ef6c1d6a83f6d16cdc5c561ccf158063ca21d22a5d2ef8f68c41ca508cbd4e7e'
REAR = 'release-tuning/ov8856.yaml'
METADATA = {'MODIFICATIONS.json', 'COMPONENTS.json', 'README.txt'}
APPLEDOUBLE = {'release-tuning/._' + name + '.yaml' for name in ('ov8856', 'ov02a10', 'uncalibrated')}
MAX_ARCHIVE = 256 * 1024 * 1024
MAX_EXPANDED = 512 * 1024 * 1024


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, indent=2, sort_keys=True) + '\n').encode()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate JSON key')
        result[key] = value
    return result


def relative(name):
    path = PurePosixPath(name)
    require(isinstance(name, str) and name and not path.is_absolute() and
            str(path) == name and not any(part in ('', '.', '..') for part in name.split('/')) and
            '\\' not in name and not any(ord(c) < 32 or ord(c) == 127 for c in name),
            'Unsafe archive path')
    return name


def read_regular(path, limit):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, 'rb') as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_size <= limit, 'Unsafe or oversized input')
        data = stream.read(limit + 1)
        require(len(data) == info.st_size and len(data) <= limit, 'Input changed or grew')
    return data


def info_record(info):
    return {key: getattr(info, key) for key in
            ('type', 'mode', 'uid', 'gid', 'uname', 'gname', 'mtime', 'linkname', 'devmajor', 'devminor', 'pax_headers')}


def inspect_archive(data, expected_sha256, expected_bytes, legacy=False):
    require(len(data) == expected_bytes and sha(data) == expected_sha256, 'Complete source outer digest/size mismatch')
    entries = {}
    expanded = 0
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:xz') as archive:
        require(not archive.pax_headers, 'Unexpected global PAX metadata')
        for member in archive:
            require(len(entries) < 10000 and member.name.startswith(PREFIX), 'Unexpected archive root or member count')
            name = relative(member.name[len(PREFIX):])
            require(name not in entries, 'Duplicate archive member')
            require(member.isfile() or member.issym(), 'Unsupported archive member type')
            require(member.uid == member.gid == 0 and member.uname == member.gname == '' and member.mtime == 0,
                    'Unexpected source ownership or timestamp')
            require(member.mode in ((0o644, 0o755) if member.isfile() else (0o777,)), 'Unsafe source member mode')
            require(set(member.pax_headers) <= {'path'} and
                    member.pax_headers.get('path', member.name) == member.name, 'Unexpected PAX metadata')
            expanded += member.size
            require(0 <= member.size <= MAX_ARCHIVE and expanded <= MAX_EXPANDED, 'Oversized source contents')
            payload = archive.extractfile(member).read() if member.isfile() else None
            require(payload is None or len(payload) == member.size, 'Truncated source member')
            entries[name] = (member, payload)
    require(METADATA | {'SHA256SUMS', REAR} <= set(entries), 'Missing complete source metadata/tuning')
    for name, (member, _) in entries.items():
        for parent in PurePosixPath(name).parents:
            require(str(parent) not in entries, 'Archive member used as a parent directory')
        if member.issym():
            target = str(PurePosixPath(name).parent / relative(member.linkname))
            require(target in entries and entries[target][0].isfile(), 'Unsafe or missing symbolic link target')
    manifest = {}
    for line in entries['SHA256SUMS'][1].decode('utf-8').splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  (.+)', line)
        require(match is not None, 'Malformed internal checksum')
        digest, name = match.groups()
        relative(name)
        require(name not in manifest, 'Duplicate internal checksum')
        require(name in entries and entries[name][0].isfile() and sha(entries[name][1]) == digest,
                'Internal checksum mismatch or non-regular target')
        manifest[name] = digest
    expected = {name for name, (member, _) in entries.items() if member.isfile()} - {'SHA256SUMS'}
    require(set(manifest) == expected - (METADATA if legacy else set()), 'Incomplete internal checksum coverage')
    for name in ('MODIFICATIONS.json', 'COMPONENTS.json'):
        require(isinstance(json.loads(entries[name][1], object_pairs_hook=unique_object), dict), 'Invalid source metadata')
    return entries


def reissue_entries(before, tuning):
    old = before[REAR][1]
    require(sha(old) == OLD_TUNING_SHA and old.count(b'  - Adjust:\n') == 1, 'Unexpected original rear tuning')
    require(tuning == old.replace(b'  - Adjust:\n', b'  - Adjust:\n      gamma: 2.4\n'),
            'Candidate must change only the one rear gamma line')
    require(APPLEDOUBLE <= set(before) and
            {name for name in before if PurePosixPath(name).name.startswith('._')} == APPLEDOUBLE,
            'Unexpected AppleDouble metadata set')
    after = {name: (copy.copy(info), data) for name, (info, data) in before.items() if name not in APPLEDOUBLE}
    modifications = json.loads(before['MODIFICATIONS.json'][1], object_pairs_hook=unique_object)
    require(REAR not in modifications, 'A previous rear tuning update is already recorded')
    modifications[REAR] = {
        'date': '2026-10-08', 'previous_sha256': sha(old), 'distributed_sha256': sha(tuning),
        'source_kit_path': 'developer/tuning/ov8856.yaml',
        'change': 'Add gamma 2.4 to Adjust; AE, contrast default and front tuning remain unchanged.',
        'notice_only_added_after_build': False, 'native_code_changed': False,
        'native_rebuild_performed': False,
    }
    readme = before['README.txt'][1] + (
        '\n2026-10-08: Rear release-tuning/ov8856.yaml adds gamma 2.4, matching the source kit. '
        'All native component source files are reused byte-for-byte; no native recompilation is claimed. '
        'Three AppleDouble tuning metadata files are omitted. SHA256SUMS covers every regular member except itself. '
        'APK payload correspondence and signatures require separate verification.\n').encode()
    replacements = {REAR: tuning, 'MODIFICATIONS.json': encoded(modifications), 'README.txt': readme}
    for name, payload in replacements.items():
        info = after[name][0]
        info.size = len(payload)
        after[name] = info, payload
    sums = ''.join(sha(data) + '  ' + name + '\n' for name, (info, data) in sorted(after.items())
                   if info.isfile() and name != 'SHA256SUMS').encode()
    info = after['SHA256SUMS'][0]
    info.size = len(sums)
    after['SHA256SUMS'] = info, sums
    return after


def verify_delta(before, after, tuning):
    require(set(before) - set(after) == APPLEDOUBLE and not set(after) - set(before), 'Unexpected source member set change')
    changed = []
    for name, (info, data) in after.items():
        old_info, old_data = before[name]
        require(info_record(info) == info_record(old_info), 'Source member metadata changed: ' + name)
        if data != old_data:
            changed.append(name)
        else:
            require(info.size == old_info.size, 'Unchanged source member size changed')
    require(set(changed) == {REAR, 'MODIFICATIONS.json', 'README.txt', 'SHA256SUMS'}, 'Unexpected source payload change')
    require(after[REAR][1] == tuning, 'New complete source tuning differs')
    return sorted(changed)


def build(source, tuning_path, output, report_path):
    require(not os.path.lexists(output) and not os.path.lexists(report_path), 'Output/report already exists')
    require(output.absolute().parent.resolve(strict=True) == output.absolute().parent and
            report_path.absolute().parent.resolve(strict=True) == report_path.absolute().parent,
            'Output parent must be a canonical existing directory')
    require(output.absolute() != report_path.absolute(), 'Output and report must differ')
    source_data = read_regular(source, MAX_ARCHIVE)
    before = inspect_archive(source_data, OLD_SHA, OLD_BYTES, legacy=True)
    tuning = read_regular(tuning_path, 16384)
    after = reissue_entries(before, tuning)
    verify_delta(before, after, tuning)
    fd = os.open(output, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
    with os.fdopen(fd, 'w+b') as stream:
        with tarfile.open(fileobj=stream, mode='w:xz', format=tarfile.PAX_FORMAT, preset=1) as archive:
            for _, (info, payload) in sorted(after.items()):
                archive.addfile(info, io.BytesIO(payload) if payload is not None else None)
        stream.flush(); os.fsync(stream.fileno())
        stream.seek(0)
        new_data = stream.read(MAX_ARCHIVE + 1)
        require(len(new_data) <= MAX_ARCHIVE, 'Unexpected output size')
        checked = inspect_archive(new_data, sha(new_data), len(new_data))
        changed = verify_delta(before, checked, tuning)
        info = os.fstat(stream.fileno())
        path_info = output.lstat()
        require((info.st_dev, info.st_ino) == (path_info.st_dev, path_info.st_ino), 'Output path changed')
    report = {
        'schema': 1, 'status': 'PASS_COMPLETE_SOURCE_REAR_GAMMA_UPDATE',
        'archive': output.name, 'bytes': len(new_data), 'sha256': sha(new_data),
        'previous_archive_sha256': OLD_SHA, 'previous_archive_bytes': OLD_BYTES,
        'source_members_before': len(before), 'source_members_after': len(checked),
        'changed_regular_members': changed, 'removed_metadata_members': sorted(APPLEDOUBLE),
        'unchanged_members_bytes_and_metadata': len(checked) - len(changed),
        'internal_checksums_verified_before': sum(info.isfile() for info, _ in before.values()) - 1 - len(METADATA),
        'internal_checksums_verified_after': sum(info.isfile() for info, _ in checked.values()) - 1,
        'native_component_bytes_and_metadata_preserved': True,
        'old_rear_tuning_sha256': OLD_TUNING_SHA, 'rear_tuning_sha256': sha(tuning),
        'source_kit_tuning_byte_identical': True, 'native_build_performed': False,
        'apk_payload_or_signature_verified_by_this_tool': False, 'hardware_tested_by_this_tool': False,
        'privacy_scope': 'Only verified public source members and the exact one-line public tuning change are included.',
    }
    fd = os.open(report_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(encoded(report)); stream.flush(); os.fsync(stream.fileno())
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--tuning', type=Path, default=ROOT / 'developer/tuning/ov8856.yaml')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(build(args.source, args.tuning, args.output, args.report), indent=2))


if __name__ == '__main__':
    main()
