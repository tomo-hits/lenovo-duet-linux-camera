#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Duet camera project contributors
"""Reissue six v1.0.1 APKs with rear gamma 2.4; preserve every ELF and five APKs.

Run as root with apk-tools 3.0.8 in an isolated native packaging environment.
Never installs packages or runs package scripts. The existing signing key stays
at its local path; only its derived public identity is inspected. A failed run
keeps its new output directory for diagnosis and cannot be resumed or replaced.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import stat
import subprocess
import tempfile

HISTORY_SHA256 = '9c34c79b180b1e547cc5b3c299cd17590c739718e027f28efa9899d9fa5e8bce'
OLD_TUNING_SHA256 = 'ef6c1d6a83f6d16cdc5c561ccf158063ca21d22a5d2ef8f68c41ca508cbd4e7e'
NEW_TUNING_SHA256 = '91c6a6e64f89c6006c31504901cfbb5c3b1d28184bcc9ecd952a2dce6686c95c'
TUNING = 'usr/share/libcamera/ipa/softisp/ov8856.yaml'
SOURCE_TUNING = 'tuning/ov8856.yaml'
LIBCAMERA = ('libcamera', 'libcamera-ipa', 'libcamera-tools', 'libcamera-dev')
CHANGES = {**{name + '-0.7.2-r102.apk': '0.7.2-r103' for name in LIBCAMERA},
           'duet-camera-0.2.12-r0.apk': '0.2.13-r0',
           'duet-camera-0.2.12-r1.apk': '0.2.13-r1'}
INFO_FIELDS = {'name', 'version', 'arch', 'license', 'description', 'depends', 'provides'}
ROOT_UID = ROOT_GID = 0


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def unique(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate JSON key')
        result[key] = value
    return result


def decode(data):
    return json.loads(data, object_pairs_hook=unique)


def relative(name):
    require(isinstance(name, str) and name and '\\' not in name and
            not any(ord(c) < 32 for c in name) and
            all(p not in ('', '.', '..') for p in name.split('/')),
            'Unsafe relative path')
    require(not PurePosixPath(name).is_absolute(), 'Absolute payload path')
    return name


def read_regular(path):
    # Open once, do not follow the final link, and retain the validated bytes.
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, 'rb') as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
                'Input must be a regular, singly linked file: ' + str(path))
        data = stream.read()
        after = os.fstat(stream.fileno())
        require((info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns) ==
                (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns),
                'Input changed during read')
        return data


def create(path, data, mode=0o644):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, mode)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(data)
    os.chmod(path, mode)


def write_json(path, value):
    create(path, (json.dumps(value, indent=2, sort_keys=True) + '\n').encode())


def run(argv, **kwargs):
    return subprocess.run(list(map(str, argv)), check=True, **kwargs)


def public_identity(path, private=False):
    command = ['openssl', 'pkey', '-in', path, '-outform', 'DER']
    command += ['-pubout'] if private else ['-pubin']
    return sha(run(command, capture_output=True).stdout)


def adb(path):
    result = decode(run(['apk', 'adbdump', '--format', 'json', path],
                        capture_output=True).stdout)
    require(isinstance(result, dict), 'Expected one ADB object')
    return result


def check_info(info, expected):
    require(isinstance(info, dict), 'Missing APK info')
    for key in INFO_FIELDS:
        if key in ('depends', 'provides'):
            values = info.get(key, [])
            require(isinstance(values, list) and all(isinstance(x, str) for x in values) and
                    len(values) == len(set(values)) and sorted(values) == sorted(expected[key].split()),
                    'APK dependency/provides mismatch: ' + key)
        else:
            require(info.get(key) == expected[key], 'APK metadata mismatch: ' + key)


def adb_paths(document):
    require(set(document) <= {'info', 'paths'} and isinstance(document.get('paths'), list),
            'Unexpected APK scripts, triggers or metadata')
    nodes = {}
    for directory in document['paths']:
        require(isinstance(directory, dict) and set(directory) <= {'name', 'acl', 'files'},
                'Unexpected APK directory metadata')
        name = directory.get('name', '')
        if name:
            relative(name)
        key = name or '.'
        require(key not in nodes, 'Duplicate APK path')
        nodes[key] = {'type': 'directory', 'metadata': {k: v for k, v in directory.items() if k != 'files'}}
        for file in directory.get('files', []):
            require(isinstance(file, dict) and isinstance(file.get('name'), str) and
                    '/' not in file['name'], 'Invalid APK file metadata')
            relative(file['name'])
            path = name + '/' + file['name'] if name else file['name']
            require(path not in nodes, 'Duplicate APK file path')
            nodes[path] = {'type': 'file', 'metadata': file}
    require('.' in nodes, 'Missing APK root directory')
    return nodes


def tree(path):
    """Never follow symlinks; compare all entries, ownership and permissions."""
    result = {}
    for current, dirs, files in os.walk(path, followlinks=False):
        here = Path(current)
        candidates = [here] + [here / n for n in files] + [here / n for n in dirs if (here / n).is_symlink()]
        for item in candidates:
            name = str(item.relative_to(path))
            if name != '.':
                relative(name)
            st = item.lstat()
            entry = {'mode': stat.S_IMODE(st.st_mode), 'uid': st.st_uid, 'gid': st.st_gid}
            if stat.S_ISDIR(st.st_mode):
                entry['type'] = 'directory'
            elif stat.S_ISLNK(st.st_mode):
                target = os.readlink(item)
                require(target and not target.startswith('/') and '\\' not in target and
                        not any(ord(c) < 32 for c in target), 'Unsafe payload symlink')
                resolved = posixpath.normpath(posixpath.join(posixpath.dirname(name), target))
                require(resolved != '..' and not resolved.startswith('../'), 'Escaping payload symlink')
                entry.update(type='symlink', target=target, mtime_ns=st.st_mtime_ns)
            else:
                require(stat.S_ISREG(st.st_mode) and st.st_nlink == 1,
                        'Special file or hardlink in payload')
                data = read_regular(item)
                entry.update(type='file', sha256=sha(data), bytes=len(data), elf=data.startswith(b'\x7fELF'),
                             mtime_ns=st.st_mtime_ns)
            result[name] = entry
    return result


def file_hashes(entries):
    return {name: item['sha256'] for name, item in entries.items() if item['type'] == 'file'}


def verify_extraction(stage, document, expected_hashes):
    entries = tree(stage)
    paths = adb_paths(document)
    require(set(entries) == set(paths), 'Extracted file/directory set differs from signed APK')
    link_times = {}
    for name, entry in entries.items():
        metadata = paths[name]['metadata']
        require((entry['type'] == 'directory') == (paths[name]['type'] == 'directory'),
                'Extracted node type differs')
        acl = metadata.get('acl')
        require(isinstance(acl, dict) and set(acl) <= {'mode', 'user', 'group'} and
                acl.get('mode') == entry['mode'] and acl.get('user') == 'root' and
                acl.get('group') == 'root' and entry['uid'] == ROOT_UID and entry['gid'] == ROOT_GID,
                'Extracted ACL differs or has unsupported attributes: ' + name)
        if entry['type'] != 'directory':
            stamp = metadata.get('mtime')
            require(type(stamp) is int and stamp >= 0, 'Invalid signed file timestamp')
            if entry['type'] == 'symlink':
                # apk-tools 3.0.8 extract creates symlinks with the current time.
                # Its ADB target is a little-endian 16-bit file type + target.
                link_bytes = os.fsencode(entry['target'])
                target = stat.S_IFLNK.to_bytes(2, 'little') + link_bytes
                require(metadata.get('target') == target.hex() and metadata.get('size') == len(link_bytes),
                        'Extracted symlink target differs from signed APK')
                link_times[name] = stamp * 1_000_000_000
            else:
                require('target' not in metadata and metadata.get('size') == entry['bytes'] and
                        metadata.get('hash') == entry['sha256'] and entry['mtime_ns'] == stamp * 1_000_000_000,
                        'Extracted regular file bytes or timestamp differs from signed APK')
    require(file_hashes(entries) == expected_hashes, 'Extracted payload hash set differs')
    # Normalize only after every path, type, target, ACL and regular byte passed.
    # Never follow a link or modify its target. Keep the strict ADB comparison.
    normalized = copy.deepcopy(entries)
    for name, stamp_ns in link_times.items():
        path = stage / name
        before = path.lstat()
        require(stat.S_ISLNK(before.st_mode) and os.readlink(path) == entries[name]['target'],
                'Symlink changed before timestamp restoration')
        if before.st_mtime_ns != stamp_ns:
            os.utime(path, ns=(before.st_atime_ns, stamp_ns), follow_symlinks=False)
        after = path.lstat()
        require((before.st_dev, before.st_ino, before.st_mode, before.st_uid, before.st_gid) ==
                (after.st_dev, after.st_ino, after.st_mode, after.st_uid, after.st_gid) and
                after.st_mtime_ns == stamp_ns, 'Symlink changed during timestamp restoration')
        normalized[name]['mtime_ns'] = stamp_ns
    actual = tree(stage)
    require(actual == normalized, 'Payload changed during timestamp restoration')
    return actual


def compare_documents(before, after, expected, changed):
    check_info(after['info'], expected)
    # Automatic archive hashes/sizes may change; all other metadata must remain.
    ignored = {'hashes', 'installed-size', 'file-size', 'version', 'depends'}
    require({k: v for k, v in before['info'].items() if k not in ignored} ==
            {k: v for k, v in after['info'].items() if k not in ignored},
            'Unexpected APK metadata change')
    old, new = adb_paths(before), adb_paths(after)
    require(old.keys() == new.keys(), 'APK paths changed')
    for name in old:
        left, right = copy.deepcopy(old[name]), copy.deepcopy(new[name])
        if name in changed:
            for key in ('hash', 'size'):
                left['metadata'].pop(key, None)
                right['metadata'].pop(key, None)
        require(left == right, 'APK attributes changed: ' + name)


def new_info(filename, old):
    info = {k: v for k, v in old.items() if k != 'sha256'}
    if filename in CHANGES:
        info['version'] = CHANGES[filename]
        info['depends'] = ' '.join(token.replace('=0.7.2-r102', '=0.7.2-r103')
                                   if token.split('=', 1)[0] in LIBCAMERA else token
                                   for token in info['depends'].split())
    return info


def package(args):
    require(os.geteuid() == 0, 'Use an isolated native root packaging environment')
    require('apk-tools 3.0.8' in run(['apk', '--version'], capture_output=True, text=True).stdout,
            'apk-tools 3.0.8 is required')
    kit = Path(__file__).resolve().parent.parent
    history_bytes = read_regular(kit / 'docs/PRE_REAR_BRIGHTNESS_PACKAGES.json')
    require(sha(history_bytes) == HISTORY_SHA256, 'Historical package record differs')
    history = decode(history_bytes)
    baseline = history['packages']
    require(history['schema'] == 1 and history['source_release'] == 'v1.0.1' and
            len(baseline) == 11 and set(CHANGES) <= baseline.keys(), 'Wrong historical package set')
    source = args.accepted_repository.resolve(strict=True)
    require(source.is_dir(), 'Input repository is not a directory')
    manifest = read_regular(source / 'packages.json')
    payload_bytes = read_regular(source / 'payload-hashes.json')
    require(sha(manifest) == history['packages_sha256'] and decode(manifest) == baseline and
            sha(payload_bytes) == history['payload_sha256'], 'Historical input metadata differs')
    payloads = decode(payload_bytes)
    require(isinstance(payloads, dict) and set(payloads) == set(baseline), 'Wrong payload package set')
    for filename, files in payloads.items():
        relative(filename)
        require(isinstance(files, dict), 'Wrong payload map')
        for name, digest in files.items():
            relative(name)
            require(isinstance(digest, str) and re.fullmatch('[0-9a-f]{64}', digest), 'Invalid payload digest')
    repo = source / 'repo/aarch64'
    require({p.name for p in repo.iterdir()} == set(baseline) | {'packages.adb'}, 'Unexpected repository members')
    public = read_regular(args.public_key)
    require(args.public_key.name == history['public_key_name'] and sha(public) == history['public_key_sha256'],
            'Historical public key differs')
    # Existing native key locations may be symlinks. Resolve once, validate the
    # explicit owner's private regular target, and never copy or print it.
    sign_key = args.sign_key.resolve(strict=True)
    sign_stat = sign_key.lstat()
    require(stat.S_ISREG(sign_stat.st_mode) and sign_stat.st_nlink == 1 and
            sign_stat.st_uid == args.sign_key_owner_uid and not sign_stat.st_mode & 0o077,
            'Local signing key must be private, owned and regular')
    require(public_identity(sign_key, private=True) == history['public_key_spki_sha256'],
            'Signing key does not match the historical public identity')
    tuning = read_regular(kit / SOURCE_TUNING)
    require(sha(tuning) == NEW_TUNING_SHA256 and
            sha(tuning.replace(b'      gamma: 2.4\n', b'')) == OLD_TUNING_SHA256 and
            tuning.count(b'      gamma: 2.4\n') == 1, 'Candidate is not the reviewed one-line tuning change')
    output = args.output.absolute()
    require(not os.path.lexists(output), 'Output must be a new directory')
    with tempfile.TemporaryDirectory(prefix='duet-brightness-input-') as temporary:
        snapshot = Path(temporary)
        keys = snapshot / 'keys'; keys.mkdir()
        create(keys / args.public_key.name, public)
        require(public_identity(keys / args.public_key.name) == history['public_key_spki_sha256'],
                'Public key identity differs')
        documents = {}
        for filename, info in baseline.items():
            require(set(info) == INFO_FIELDS | {'sha256'} and
                    filename == info['name'] + '-' + info['version'] + '.apk' and info['arch'] == 'aarch64',
                    'Invalid fixed package identity')
            data = read_regular(repo / filename)
            require(sha(data) == info['sha256'], 'Input APK hash differs: ' + filename)
            create(snapshot / filename, data)
            run(['apk', 'verify', '--keys-dir', keys, snapshot / filename])
            document = adb(snapshot / filename)
            check_info(document.get('info'), info)
            adb_paths(document)  # Reject paths/scripts before extraction.
            documents[filename] = document
        index = read_regular(repo / 'packages.adb')
        require(sha(index) == history['index_sha256'], 'Historical signed index differs')
        create(snapshot / 'packages.adb', index)
        run(['apk', 'verify', '--keys-dir', keys, snapshot / 'packages.adb'])
        output.mkdir(mode=0o755)
        out_repo = output / 'repo/aarch64'; out_repo.mkdir(parents=True)
        out_keys = output / 'keys'; out_keys.mkdir()
        create(out_keys / args.public_key.name, public)
        stages = output / 'stages'; stages.mkdir()
        verified = output / 'verified'; verified.mkdir()
        manifests, output_hashes, correspondence = {}, {}, {}
        for filename, old_info in baseline.items():
            before = stages / filename.removesuffix('.apk'); before.mkdir()
            run(['apk', 'extract', '--keys-dir', keys, '--destination', before, snapshot / filename])
            old_tree = verify_extraction(before, documents[filename], payloads[filename])
            info = new_info(filename, old_info)
            new_name = info['name'] + '-' + info['version'] + '.apk'
            changed = {TUNING} if info['name'] == 'libcamera-ipa' else set()
            if changed:
                target = before / TUNING
                require(old_tree[TUNING]['type'] == 'file' and old_tree[TUNING]['sha256'] == OLD_TUNING_SHA256,
                        'Original rear tuning differs')
                stamp = target.stat()
                # The full scan above rejected symlink parents, hardlinks and extras.
                with target.open('r+b') as stream:
                    stream.write(tuning); stream.truncate()
                os.utime(target, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
            intended = tree(before)
            delta = {p for p in set(old_tree) | set(intended) if old_tree.get(p) != intended.get(p)}
            require(delta == changed, 'Unexpected staging change')
            if filename in CHANGES:
                command = ['apk', 'mkpkg', '--files', before, '--output', out_repo / new_name,
                           '--sign-key', sign_key]
                for key, value in info.items():
                    command += ['--info', key + ':' + value]
                run(command)
            else:
                create(out_repo / new_name, read_regular(snapshot / filename))
            run(['apk', 'verify', '--keys-dir', out_keys, out_repo / new_name])
            after_doc = adb(out_repo / new_name)
            compare_documents(documents[filename], after_doc, info, changed)
            dest = verified / new_name.removesuffix('.apk'); dest.mkdir()
            run(['apk', 'extract', '--keys-dir', out_keys, '--destination', dest, out_repo / new_name])
            after_tree = verify_extraction(dest, after_doc, file_hashes(intended))
            require(after_tree == intended, 'Output extraction differs from intended bytes/attributes')
            elves = {p: x['sha256'] for p, x in old_tree.items() if x.get('elf')}
            require(elves == {p: x['sha256'] for p, x in after_tree.items() if x.get('elf')}, 'ELF bytes changed')
            digest = sha(read_regular(out_repo / new_name))
            if filename not in CHANGES:
                require(digest == old_info['sha256'], 'Reused APK bytes changed')
            manifests[new_name] = dict(info, sha256=digest)
            output_hashes[new_name] = file_hashes(after_tree)
            correspondence[new_name] = {'source_apk': filename, 'source_apk_sha256': old_info['sha256'],
                'apk_byte_reused': filename not in CHANGES, 'changed_regular_files': sorted(changed),
                'all_elf_payloads_byte_identical': True, 'elf_files': len(elves),
                'attributes_and_links_preserved': True,
                'source_tuning': 'developer/' + SOURCE_TUNING if changed else None,
                'source_tuning_sha256': sha(tuning) if changed else None,
                'actual_tuning_sha256': after_tree[TUNING]['sha256'] if changed else None}
        run(['apk', 'mkndx', '--sign-key', sign_key, '--output', out_repo / 'packages.adb',
             *sorted(out_repo.glob('*.apk'))])
        run(['apk', 'verify', '--keys-dir', out_keys, out_repo / 'packages.adb'])
        index_doc = adb(out_repo / 'packages.adb')
        infos = index_doc.get('packages')
        require(isinstance(infos, list) and len(infos) == 11, 'Wrong signed index package count')
        indexed = {}
        for info in infos:
            filename = info.get('name', '') + '-' + info.get('version', '') + '.apk'
            require(filename in manifests and filename not in indexed, 'Unexpected/duplicate index package')
            check_info(info, manifests[filename])
            apk_info = adb(out_repo / filename)['info']
            require({k: v for k, v in info.items() if k not in ('hashes', 'file-size')} ==
                    {k: v for k, v in apk_info.items() if k != 'hashes'} and
                    info.get('file-size') == (out_repo / filename).stat().st_size and
                    isinstance(info.get('hashes'), str) and re.fullmatch('[0-9a-f]{64}', info['hashes']),
                    'Signed index metadata differs from actual APK')
            indexed[filename] = info
        # Recompute identities from the finished APKs without the signing key.
        # ADB index identities differ from the short hash embedded in an APK.
        run(['apk', 'mkndx', '--output', verified / 'recomputed-index.adb',
             *sorted(out_repo.glob('*.apk'))])
        require(adb(verified / 'recomputed-index.adb') == index_doc,
                'Signed index differs from recomputed output package identities')
        for name, value in [('packages.json', manifests), ('payload-hashes.json', output_hashes),
                            ('correspondence.json', correspondence)]:
            write_json(output / name, value)
        result = {'signed_packages': 11, 'reissued_packages': 6, 'byte_reused_packages': 5,
                  'all_elf_payloads_byte_identical': True, 'source_tuning_sha256': sha(tuning),
                  'runtime_tuning_sha256': output_hashes['libcamera-ipa-0.7.2-r103.apk'][TUNING],
                  'source_release': 'v1.0.1', 'native_compilation_performed': False,
                  'hardware_acceptance_performed': False, 'private_key_exported': False}
        write_json(output / 'validation.json', result)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('accepted-repository', 'output', 'sign-key', 'public-key'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--sign-key-owner-uid', type=int, required=True,
                        help='Expected owner of the existing private key; never changes ownership')
    print(json.dumps(package(parser.parse_args()), sort_keys=True))


if __name__ == '__main__':
    main()
