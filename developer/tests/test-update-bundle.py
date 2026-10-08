#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Duet camera project contributors
"""Exercise archive contents and rejection paths using synthetic release inputs."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/build-update-bundle.py'
SPEC = importlib.util.spec_from_file_location('bundle_under_test', SCRIPT)
bundle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bundle)
DER = b'synthetic public-key DER fixture; no real key'
KEY_HASH = hashlib.sha256(DER).hexdigest()
PREFIX = 'lenovo-duet-linux-camera/'


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


class BundleTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='duet-update-bundle-')
        self.addCleanup(temporary.cleanup)
        self.temp = Path(temporary.name)
        self.root = self.temp / 'source'
        self.packages = self.temp / 'packages'
        self.full_source = self.temp / 'complete.tar.xz'
        self.tuning = {name: ('fixture tuning ' + name).encode() for name in
                       ('ov02a10.yaml', 'ov8856.yaml', 'uncalibrated.yaml')}
        self.write_full_source(self.tuning)
        self.output = self.temp / 'bundle.tar'
        versions = [*bundle.update.PINS.items(), ('libcamera-dev', bundle.update.PINS['libcamera']),
                    ('duet-camera', bundle.update.PINS['duet-camera'].replace('-r1', '-r0')), ('duet-camera-config', '0.1.3-r0')]
        self.manifest = {}
        for name, version in versions:
            filename = name + '-' + version + '.apk'
            payload = ('synthetic package ' + filename).encode()
            write(self.packages / 'repo/aarch64' / filename, payload)
            self.manifest[filename] = {
                'name': name, 'version': version, 'arch': 'aarch64',
                'sha256': hashlib.sha256(payload).hexdigest(),
            }
        write(self.packages / 'packages.json', json.dumps(self.manifest).encode())
        write(self.packages / 'keys/fixture.rsa.pub', b'synthetic public key')
        write(self.packages / 'repo/aarch64/packages.adb', b'synthetic package index')
        self.payloads = {'libcamera-ipa-' + bundle.update.PINS['libcamera-ipa'] + '.apk': {
            'usr/share/libcamera/ipa/softisp/' + name: hashlib.sha256(data).hexdigest()
            for name, data in self.tuning.items()}}
        write(self.packages / 'payload-hashes.json', json.dumps(self.payloads).encode())
        self.sources = {
            'install.sh': b'#!/bin/sh\necho install\n',
            'restore.sh': b'#!/bin/sh\necho restore\n',
            'lib/entrypoint.py': b'# fixture entry point\n',
            'developer/scripts/apply-update.py': b'# fixture updater\n',
            'developer/docs/PUBLIC_PACKAGES.json': json.dumps(self.manifest).encode(),
            'developer/docs/COMPLETE_SOURCES.json': json.dumps({
                'archive': 'complete.tar.xz', 'bytes': self.full_source.stat().st_size,
                'sha256': hashlib.sha256(self.full_source.read_bytes()).hexdigest(),
            }).encode(),
        }
        self.sources.update({'developer/tuning/' + name: data for name, data in self.tuning.items()})
        for name, data in self.sources.items():
            write(self.root / name, data)
        (self.root / 'install.sh').chmod(0o755)
        sums = ''.join(hashlib.sha256(data).hexdigest() + '  ' + name + '\n'
                       for name, data in self.sources.items())
        write(self.root / 'SHA256SUMS', sums.encode())

    def write_full_source(self, tuning, duplicate=False, symlink=False):
        with tarfile.open(self.full_source, 'w:xz') as archive:
            for name, data in tuning.items():
                item = tarfile.TarInfo('mt8183-camera-complete-sources/release-tuning/' + name)
                item.size = len(data)
                if symlink and name == 'ov8856.yaml':
                    item.type = tarfile.SYMTYPE
                    item.linkname = 'ov02a10.yaml'
                    item.size = 0
                    archive.addfile(item)
                else:
                    archive.addfile(item, io.BytesIO(data))
                if duplicate and name == 'ov8856.yaml':
                    archive.addfile(item, io.BytesIO(data))

    def acknowledge_full_source(self):
        name = 'developer/docs/COMPLETE_SOURCES.json'
        self.sources[name] = json.dumps({
            'archive': 'complete.tar.xz', 'bytes': self.full_source.stat().st_size,
            'sha256': hashlib.sha256(self.full_source.read_bytes()).hexdigest(),
        }).encode()
        write(self.root / name, self.sources[name])
        self.refresh_checksums()

    def refresh_checksums(self):
        sums = ''.join(hashlib.sha256(data).hexdigest() + '  ' + name + '\n'
                       for name, data in self.sources.items())
        write(self.root / 'SHA256SUMS', sums.encode())

    def test_updated_source_with_stale_apk_tuning_is_rejected(self):
        name = 'developer/tuning/ov8856.yaml'
        self.sources[name] += b'new gamma'
        write(self.root / name, self.sources[name])
        self.refresh_checksums()
        self.assert_rejected('APK payload tuning differs from source')

    def test_updated_apk_and_source_with_stale_complete_tuning_is_rejected(self):
        name = 'developer/tuning/ov8856.yaml'
        self.sources[name] += b'new gamma'
        write(self.root / name, self.sources[name])
        self.refresh_checksums()
        next(iter(self.payloads.values()))['usr/share/libcamera/ipa/softisp/ov8856.yaml'] = hashlib.sha256(self.sources[name]).hexdigest()
        write(self.packages / 'payload-hashes.json', json.dumps(self.payloads).encode())
        self.assert_rejected('Invalid complete-source tuning member|Complete-source tuning differs')

    def test_valid_outer_hash_does_not_hide_missing_tuning(self):
        self.write_full_source({k: v for k, v in self.tuning.items() if k != 'ov8856.yaml'})
        self.acknowledge_full_source()
        self.assert_rejected('Missing complete-source tuning')

    def test_duplicate_complete_source_member_is_rejected(self):
        self.write_full_source(self.tuning, duplicate=True)
        self.acknowledge_full_source()
        self.assert_rejected('Duplicate complete-source member')

    def test_symlink_complete_tuning_is_rejected(self):
        self.write_full_source(self.tuning, symlink=True)
        self.acknowledge_full_source()
        self.assert_rejected('Invalid complete-source tuning member')

    def assemble(self, key=KEY_HASH):
        with mock.patch.object(bundle.subprocess, 'run', return_value=
                               subprocess.CompletedProcess(['openssl'], 0, stdout=DER)) as command:
            result = bundle.assemble(self.root, self.packages, self.full_source, self.output, key)
        command.assert_called_once()
        self.assertEqual(command.call_args.args[0][:2], ['openssl', 'pkey'])
        return result

    def assert_rejected(self, message):
        with self.assertRaisesRegex(ValueError, message):
            self.assemble()
        self.assertFalse(self.output.exists())

    def test_archive_is_allowlisted_complete_and_checksums_match(self):
        for name in ('private.rsa', '.git/config', 'developer/work/private.pem', 'build/output.ko'):
            write(self.root / name, b'UNLISTED PRIVATE FIXTURE')
        for name in ('keys/private.rsa', 'stages/private.pem', 'repo/aarch64/unlisted.apk'):
            write(self.packages / name, b'UNLISTED PRIVATE FIXTURE')
        record = self.assemble()
        with tarfile.open(self.output) as archive:
            members = archive.getmembers()
            self.assertTrue(all(member.isfile() for member in members))
            contents = {member.name.removeprefix(PREFIX): archive.extractfile(member).read()
                        for member in members}
            self.assertEqual(archive.getmember(PREFIX + 'install.sh').mode, 0o755)
        expected = set(self.sources) | {
            'SHA256SUMS', 'BUNDLE.json', 'BUNDLE_SHA256SUMS', 'sources/complete.tar.xz',
            'packages/packages.json', 'packages/keys/fixture.rsa.pub',
            'packages/repo/aarch64/packages.adb', 'packages/payload-hashes.json',
        } | {'packages/repo/aarch64/' + name for name in self.manifest}
        self.assertEqual(set(contents), expected)
        self.assertEqual(contents['sources/complete.tar.xz'], self.full_source.read_bytes())
        for line in contents['BUNDLE_SHA256SUMS'].decode().splitlines():
            digest, name = line.split('  ', 1)
            self.assertEqual(hashlib.sha256(contents[name]).hexdigest(), digest)
        self.assertEqual(record['sha256'], hashlib.sha256(self.output.read_bytes()).hexdigest())
        self.assertEqual(record['files'], len(contents))
        self.assertFalse(record['author_recorded_apks_byte_identical'])
        self.assertIsNone(record['accepted_package_manifest'])
        self.assertFalse(record['hardware_acceptance'])

    def test_changed_source_is_rejected(self):
        (self.root / 'install.sh').write_bytes(b'tampered')
        self.assert_rejected('Source digest mismatch')

    def test_changed_package_is_rejected(self):
        next((self.packages / 'repo/aarch64').glob('*.apk')).write_bytes(b'tampered')
        self.assert_rejected('Package digest mismatch')

    def test_optional_official_zstd_is_separate_and_only_pinned_files_are_copied(self):
        directory = self.packages/'official-zstd'
        apk = directory/('zstd-'+bundle.update.OFFICIAL_ZSTD['version']+'.apk')
        index = directory/'APKINDEX.tar.gz'
        write(apk, b'official package fixture'); write(index, b'official signed index fixture')
        write(directory/'unlisted-private', b'do not include')
        policy = dict(bundle.update.OFFICIAL_ZSTD, apk_sha256=hashlib.sha256(apk.read_bytes()).hexdigest(),
                      index_sha256=hashlib.sha256(index.read_bytes()).hexdigest())
        with mock.patch.object(bundle.update, 'OFFICIAL_ZSTD', policy):
            record = self.assemble()
        self.assertEqual(record['optional_official_packages'], ['zstd='+policy['version']])
        with tarfile.open(self.output) as archive:
            names = [name for name in archive.getnames() if '/official-zstd/' in name]
        self.assertEqual(set(names), {PREFIX+'packages/official-zstd/'+p.name for p in (apk, index)})

    def test_unpinned_official_supplement_is_rejected(self):
        directory = self.packages/'official-zstd'
        write(directory/('zstd-'+bundle.update.OFFICIAL_ZSTD['version']+'.apk'), b'wrong APK')
        write(directory/'APKINDEX.tar.gz', b'wrong index')
        self.assert_rejected('Official supplement digest mismatch')

    def test_wrong_complete_source_is_rejected(self):
        self.full_source.write_bytes(b'wrong source archive')
        self.assert_rejected('Complete corresponding source archive mismatch')

    def test_existing_output_is_preserved(self):
        self.output.write_bytes(b'previous candidate')
        with self.assertRaises(FileExistsError):
            self.assemble()
        self.assertEqual(self.output.read_bytes(), b'previous candidate')

    def test_output_symlink_target_is_preserved(self):
        target = self.temp / 'previous.tar'
        target.write_bytes(b'previous candidate')
        self.output.symlink_to(target)
        with self.assertRaises(FileExistsError):
            self.assemble()
        self.assertTrue(self.output.is_symlink())
        self.assertEqual(target.read_bytes(), b'previous candidate')

    def test_source_file_symlink_is_rejected(self):
        target = self.temp / 'install.sh'
        (self.root / 'install.sh').rename(target)
        (self.root / 'install.sh').symlink_to(target)
        self.assert_rejected('regular input|[Ss]ymlink')

    def test_source_directory_symlink_is_rejected(self):
        target = self.temp / 'outside-docs'
        shutil.move(str(self.root / 'developer/docs'), target)
        (self.root / 'developer/docs').symlink_to(target, target_is_directory=True)
        self.assert_rejected('regular input|[Ss]ymlink')

    def test_wrong_public_key_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Signing public key mismatch'):
            self.assemble('0' * 64)
        self.assertFalse(self.output.exists())

    def test_author_release_requires_exact_recorded_package_set(self):
        accepted = dict(self.manifest)
        name = next(iter(accepted))
        accepted[name] = {**accepted[name], 'version': 'different'}
        data = json.dumps(accepted).encode()
        (self.root / 'developer/docs/PUBLIC_PACKAGES.json').write_bytes(data)
        lines = (self.root / 'SHA256SUMS').read_text().splitlines()
        sums = '\n'.join(hashlib.sha256(data).hexdigest() + '  developer/docs/PUBLIC_PACKAGES.json'
                         if line.endswith('  developer/docs/PUBLIC_PACKAGES.json') else line for line in lines)
        (self.root / 'SHA256SUMS').write_text(sums + '\n')
        with mock.patch.object(bundle.update, 'KEY_HASH', KEY_HASH):
            self.assert_rejected('Author-key release differs')


if __name__ == '__main__':
    unittest.main()
