#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Duet camera project contributors
"""Exercise package-low-fps-fix.py with disposable, self-contained host fixtures.

Production main, validation, path handling and writes run unchanged. Only the
pinned fixture inputs and external apk/modinfo/openssl commands are mocked.
No packages, real signing keys, device, network or native tools are required.
Use unittest checks so the same assertions remain active under python -O.
"""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/package-low-fps-fix.py'
spec = importlib.util.spec_from_file_location('package_low_fps_under_test', SCRIPT)
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)
MODULE_APK = 'duet-camera-modules-0.2.3-r1.apk'
NEW_MODULE_APK = 'duet-camera-modules-0.2.4-r0.apk'
P1_PATH = 'lib/modules/6.18.28-mt81/extra/duet-camera/mt8183_p1.ko'
KEEP_PATH = 'usr/share/doc/fixture/notice.txt'
OLD = b'original fixture P1 module'
GOOD = (b'\x7fELF\x02\x01\x01' + b'\0' * 9 + struct.pack('<HH', 1, 183)
        + b'verified fixture P1 module')
LATER = b'module input changed after validation; not an ELF'
PUBLIC_DER = b'public fixture identity, not a real key'


def sha(data):
    return hashlib.sha256(data).hexdigest()


class PackagePaths(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='duet-package-paths-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.kit = self.root / 'kit'
        (self.kit / 'scripts').mkdir(parents=True)
        (self.kit / 'docs').mkdir()
        self.source = self.root / 'accepted'
        (self.source / 'repo/aarch64').mkdir(parents=True)
        self.output = self.root / 'output'
        self.module = self.root / 'input.ko'
        self.module.write_bytes(GOOD)
        self.public = self.root / 'fixture.pub'
        self.public.write_bytes(b'public fixture placeholder')
        self.sign_key = self.root / 'fixture-sign-key'
        self.sign_key.write_bytes(b'fixture placeholder; not a private key')
        self.victim = self.root / 'outside/mt8183_p1.ko'
        self.victim.parent.mkdir()
        self.victim.write_bytes(OLD)
        versions = [('duet-camera-modules', '0.2.3-r1'),
                    ('duet-camera', '0.2.5-r0'),
                    ('duet-camera', '0.2.5-r1')]
        versions += [('unchanged-' + str(n), '1.0-r0') for n in range(8)]
        self.baseline, self.files, self.apk_bytes = {}, {}, {}
        for name, version in versions:
            filename = name + '-' + version + '.apk'
            data = ('fixture APK ' + filename).encode()
            self.apk_bytes[filename] = data
            (self.source / 'repo/aarch64' / filename).write_bytes(data)
            self.baseline[filename] = {
                'name': name, 'version': version, 'arch': 'aarch64',
                'license': 'MIT', 'description': 'Self-contained test fixture',
                'depends': 'duet-camera-modules=0.2.3-r1',
                'provides': '', 'sha256': sha(data),
            }
            self.files[filename] = {KEEP_PATH: ('preserve ' + filename).encode()}
        self.files[MODULE_APK][P1_PATH] = OLD
        self.payloads = {filename: {path: sha(data) for path, data in files.items()}
                         for filename, files in self.files.items()}
        (self.kit / 'docs/PRE_S1_PACKAGES.json').write_text(json.dumps(self.baseline))
        self.manifest = self.source / 'payload-hashes.json'
        self.manifest.write_text(json.dumps(self.payloads))
        self.manifest_pin = sha(self.manifest.read_bytes())
        self.extraction_fault = None
        self.change_input_after_validation = False
        self.expected_module_hash = sha(GOOD)
        self.calls = []

    def run_main(self):
        argv = [str(SCRIPT), '--accepted-repository', str(self.source),
                '--output', str(self.output), '--sign-key', str(self.sign_key),
                '--public-key', str(self.public), '--p1-module', str(self.module),
                '--p1-sha256', self.expected_module_hash]
        output = io.StringIO()
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(packager, '__file__', str(self.kit / 'scripts/package-low-fps-fix.py')))
            stack.enter_context(mock.patch.object(packager, 'PRE_S1_PAYLOAD_SHA256', self.manifest_pin))
            stack.enter_context(mock.patch.object(packager, 'PUBLIC_KEY_SHA256', sha(PUBLIC_DER)))
            stack.enter_context(mock.patch.object(sys, 'argv', argv))
            stack.enter_context(mock.patch.object(packager.subprocess, 'check_output', self.check_output))
            stack.enter_context(mock.patch.object(packager.subprocess, 'run', self.run_command))
            stack.enter_context(contextlib.redirect_stdout(output))
            packager.main()
        return json.loads(output.getvalue())

    def check_output(self, argv, **kwargs):
        self.calls.append(tuple(map(str, argv)))
        if str(argv[0]) == '/sbin/modinfo':
            snapshot = Path(argv[1])
            self.assertNotEqual(snapshot, self.module)
            self.assertEqual(snapshot.read_bytes(), GOOD)
            return 'vermagic: 6.18.28-mt81 SMP preempt mod_unload aarch64\nname: mt8183_p1\n'
        if str(argv[0]) == 'openssl':
            return PUBLIC_DER
        raise RuntimeError('Unexpected external command in host test')

    def run_command(self, argv, **kwargs):
        self.calls.append(tuple(map(str, argv)))
        if argv[0] != 'apk':
            raise RuntimeError('Unexpected external command in host test')
        operation = argv[1]
        if operation == 'verify':
            if self.change_input_after_validation:
                self.module.write_bytes(LATER)
        elif operation == 'extract':
            destination = Path(argv[argv.index('--destination') + 1])
            filename = Path(argv[-1]).name
            for relative, data in self.files[filename].items():
                target = destination / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            if filename == MODULE_APK:
                target = destination / P1_PATH
                if self.extraction_fault == 'parent-symlink':
                    target.unlink()
                    target.parent.rmdir()
                    target.parent.symlink_to(self.victim.parent, target_is_directory=True)
                elif self.extraction_fault == 'file-symlink':
                    target.unlink()
                    target.symlink_to(self.victim)
                elif self.extraction_fault == 'hardlink':
                    target.unlink()
                    os.link(self.victim, target)
                elif self.extraction_fault == 'wrong-payload':
                    target.write_bytes(b'incorrect extraction')
                elif self.extraction_fault == 'extra-payload':
                    (destination / 'unlisted-file').write_bytes(b'unlisted')
        elif operation in ('mkpkg', 'mkndx'):
            Path(argv[argv.index('--output') + 1]).write_bytes(b'synthetic signed output')
        else:
            raise RuntimeError('Unexpected apk operation in host test')
        return subprocess.CompletedProcess(argv, 0)

    def set_payloads(self, payloads, pin=True):
        self.manifest.write_text(json.dumps(payloads))
        if pin:
            self.manifest_pin = sha(self.manifest.read_bytes())

    def assert_rejected_before_output(self):
        with self.assertRaises((ValueError, OSError)):
            self.run_main()
        self.assertFalse(self.output.exists())
        self.assertFalse(self.output.is_symlink())
        self.assertEqual(self.victim.read_bytes(), OLD)
        self.assertFalse(any(call[:2] == ('apk', 'extract') for call in self.calls))

    def test_valid_input_changes_only_fixed_p1_path(self):
        result = self.run_main()
        self.assertEqual(result['signed_packages'], 11)
        self.assertEqual(result['reissued_packages'], 3)
        self.assertEqual(result['unchanged_packages'], 8)
        self.assertTrue(result['only_p1_elf_changed'])
        stage = self.output / 'stages' / NEW_MODULE_APK.removesuffix('.apk')
        self.assertEqual((stage / P1_PATH).read_bytes(), GOOD)
        self.assertEqual((stage / KEEP_PATH).read_bytes(), self.files[MODULE_APK][KEEP_PATH])
        record = json.loads((self.output / 'correspondence.json').read_text())
        self.assertEqual(record[NEW_MODULE_APK]['changed_payload_files'], [P1_PATH])
        hashes = json.loads((self.output / 'payload-hashes.json').read_text())
        self.assertEqual(hashes[NEW_MODULE_APK][P1_PATH], sha(GOOD))
        for filename, original in self.apk_bytes.items():
            if filename.startswith('unchanged-'):
                self.assertEqual((self.output / 'repo/aarch64' / filename).read_bytes(), original)
        self.assertEqual(self.victim.read_bytes(), OLD)

    def test_wrong_module_hash_is_rejected_before_output(self):
        self.expected_module_hash = '0' * 64
        self.assert_rejected_before_output()

    def test_wrong_module_elf_is_rejected_before_output(self):
        self.module.write_bytes(LATER)
        self.expected_module_hash = sha(LATER)
        self.assert_rejected_before_output()

    def test_wrong_baseline_apk_hash_is_rejected_before_output(self):
        (self.source / 'repo/aarch64' / MODULE_APK).write_bytes(b'changed APK')
        self.assert_rejected_before_output()

    def test_manifest_pin_rejects_changed_bytes(self):
        self.manifest.write_bytes(self.manifest.read_bytes() + b'\n')
        self.assert_rejected_before_output()

    def test_absolute_path_rejected_before_output(self):
        payloads = copy.deepcopy(self.payloads)
        payloads[MODULE_APK] = {str(self.victim): sha(OLD)}
        self.set_payloads(payloads)
        self.assert_rejected_before_output()

    def test_noncanonical_paths_rejected_before_output(self):
        cases = ['../outside/mt8183_p1.ko',
                 'lib/../../outside/mt8183_p1.ko',
                 './' + P1_PATH, P1_PATH.replace('lib/', 'lib//', 1),
                 P1_PATH + '/', '', '.', 'a/./mt8183_p1.ko',
                 'a/../mt8183_p1.ko', 'lib\\mt8183_p1.ko',
                 'lib/mt8183_p1.ko\x00']
        for relative in cases:
            with self.subTest(path=repr(relative)):
                payloads = copy.deepcopy(self.payloads)
                payloads[MODULE_APK][relative] = sha(OLD)
                self.set_payloads(payloads)
                self.assert_rejected_before_output()

    def test_manifest_package_set_and_shapes_rejected_before_output(self):
        cases = []
        missing = copy.deepcopy(self.payloads)
        missing.pop('unchanged-7-1.0-r0.apk')
        cases.append(missing)
        extra = copy.deepcopy(self.payloads)
        extra['../unexpected.apk'] = {}
        cases.append(extra)
        shape = copy.deepcopy(self.payloads)
        shape[MODULE_APK] = []
        cases += [shape, [], None]
        for payloads in cases:
            with self.subTest(payloads=type(payloads).__name__):
                self.set_payloads(payloads)
                self.assert_rejected_before_output()

    def test_hash_values_rejected_before_output(self):
        for expected in ['0' * 63, 'G' * 64, 'A' * 64, '', None, 1, []]:
            with self.subTest(expected=expected):
                payloads = copy.deepcopy(self.payloads)
                payloads[MODULE_APK][P1_PATH] = expected
                self.set_payloads(payloads)
                self.assert_rejected_before_output()

    def test_unchanged_package_paths_are_also_validated(self):
        payloads = copy.deepcopy(self.payloads)
        payloads['unchanged-7-1.0-r0.apk']['../outside/mt8183_p1.ko'] = sha(OLD)
        self.set_payloads(payloads)
        self.assert_rejected_before_output()

    def test_nonfixed_p1_destination_rejected_before_output(self):
        payloads = copy.deepcopy(self.payloads)
        payloads[MODULE_APK]['other/mt8183_p1.ko'] = payloads[MODULE_APK].pop(P1_PATH)
        self.set_payloads(payloads)
        self.assert_rejected_before_output()

    def test_duplicate_json_keys_rejected_before_output(self):
        original = json.dumps(self.payloads)
        duplicate_package = original[:-1] + ', ' + json.dumps(MODULE_APK) + ': {}}'
        entry = json.dumps(P1_PATH) + ': ' + json.dumps(sha(OLD))
        duplicate_path = original.replace(entry, entry + ', ' + entry)
        for raw in [duplicate_package, duplicate_path]:
            with self.subTest(raw_length=len(raw)):
                self.manifest.write_text(raw)
                self.manifest_pin = sha(self.manifest.read_bytes())
                self.assert_rejected_before_output()

    def test_existing_output_is_preserved(self):
        self.output.mkdir()
        sentinel = self.output / 'sentinel'
        sentinel.write_bytes(OLD)
        with self.assertRaises((ValueError, OSError)):
            self.run_main()
        self.assertEqual(sentinel.read_bytes(), OLD)
        self.assertEqual(list(self.output.iterdir()), [sentinel])

    def test_output_symlink_is_preserved(self):
        for dangling in [False, True]:
            with self.subTest(dangling=dangling):
                target = self.victim.parent if not dangling else self.root / 'absent'
                self.output.symlink_to(target, target_is_directory=True)
                with self.assertRaises((ValueError, OSError)):
                    self.run_main()
                self.assertTrue(self.output.is_symlink())
                self.assertEqual(self.victim.read_bytes(), OLD)
                self.assertFalse((self.root / 'absent').exists())
                self.output.unlink()

    def test_extracted_symlinks_and_hardlinks_cannot_modify_external_file(self):
        for fault in ['parent-symlink', 'file-symlink', 'hardlink']:
            with self.subTest(fault=fault):
                self.extraction_fault = fault
                self.output = self.root / ('output-' + fault)
                with self.assertRaises((ValueError, OSError)):
                    self.run_main()
                self.assertEqual(self.victim.read_bytes(), OLD)
                self.assertFalse((self.output / 'repo/aarch64' / NEW_MODULE_APK).exists())

    def test_extracted_payload_mismatch_is_rejected(self):
        self.extraction_fault = 'wrong-payload'
        with self.assertRaises((ValueError, OSError)):
            self.run_main()
        self.assertEqual(self.victim.read_bytes(), OLD)
        self.assertFalse((self.output / 'repo/aarch64' / NEW_MODULE_APK).exists())

    def test_unlisted_extracted_file_is_rejected_before_replacement(self):
        self.extraction_fault = 'extra-payload'
        with self.assertRaises((ValueError, OSError)):
            self.run_main()
        stage = self.output / 'stages' / NEW_MODULE_APK.removesuffix('.apk')
        self.assertEqual((stage / P1_PATH).read_bytes(), OLD)
        self.assertFalse((self.output / 'repo/aarch64' / NEW_MODULE_APK).exists())

    def test_verified_module_snapshot_survives_later_input_change(self):
        self.change_input_after_validation = True
        result = self.run_main()
        stage = self.output / 'stages' / NEW_MODULE_APK.removesuffix('.apk')
        self.assertEqual(self.module.read_bytes(), LATER)
        self.assertEqual((stage / P1_PATH).read_bytes(), GOOD)
        self.assertEqual(result['p1_sha256'], sha(GOOD))
        hashes = json.loads((self.output / 'payload-hashes.json').read_text())
        self.assertEqual(hashes[NEW_MODULE_APK][P1_PATH], sha(GOOD))


if __name__ == '__main__':
    unittest.main(verbosity=2)
