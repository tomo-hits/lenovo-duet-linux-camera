#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Signing-key policy at the real APK selection boundary; no package manager runs."""
import argparse
import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/apply-update.py'
spec = importlib.util.spec_from_file_location('update_key_under_test', SCRIPT)
update = importlib.util.module_from_spec(spec)
spec.loader.exec_module(update)


class SigningKeyPolicy(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        (self.root/'keys').mkdir()
        (self.root/'repo/aarch64').mkdir(parents=True)
        manifest = {}
        for name, version in update.PINS.items():
            filename = name+'-'+version+'.apk'
            data = filename.encode()
            (self.root/'repo/aarch64'/filename).write_bytes(data)
            manifest[filename] = dict(name=name, version=version, arch='aarch64',
                                      sha256=hashlib.sha256(data).hexdigest())
        (self.root/'packages.json').write_text(json.dumps(manifest))
        self.der = b'Developer public-key DER fixture'
        self.fingerprint = hashlib.sha256(self.der).hexdigest()
        self.pem = b'-----BEGIN PUBLIC KEY-----\n'+base64.b64encode(self.der)+b'\n-----END PUBLIC KEY-----\n'
        (self.root/'keys/local.pub').write_bytes(self.pem)

    def test_unselected_developer_key_is_rejected_before_apk_verify(self):
        with patch.object(update, 'run', return_value=subprocess.CompletedProcess([], 0, stdout=self.der)) as run:
            with self.assertRaisesRegex(ValueError, 'Signing public key mismatch'):
                update.selected_apks(self.root)
            run.assert_not_called()

    def test_explicit_expected_key_still_requires_apk_verification(self):
        with patch.object(update, 'run', return_value=subprocess.CompletedProcess([], 0, stdout=self.der)) as run:
            selected, key = update.selected_apks(self.root, self.fingerprint)
            self.assertEqual(len(selected), len(update.PINS))
            self.assertEqual(key, self.root/'keys/local.pub')
            self.assertEqual(run.call_args.args[0][:2], ['apk', 'verify'])

    def test_signature_failure_is_not_ignored(self):
        with patch.object(update, 'run', side_effect=subprocess.CalledProcessError(1, ['apk', 'verify'])):
            with self.assertRaises(subprocess.CalledProcessError):
                update.selected_apks(self.root, self.fingerprint)

    def test_digest_mismatch_is_not_ignored(self):
        next((self.root/'repo/aarch64').glob('*.apk')).write_bytes(b'changed')
        with patch.object(update, 'run', return_value=subprocess.CompletedProcess([], 0, stdout=self.der)) as run:
            with self.assertRaisesRegex(ValueError, 'Package digest mismatch'):
                update.selected_apks(self.root, self.fingerprint)
            run.assert_not_called()

    def test_spki_pem_line_endings_and_final_newline(self):
        for pem in (self.pem, self.pem.rstrip(b'\n'), self.pem.replace(b'\n', b'\r\n')):
            with self.subTest(pem=pem):
                self.assertEqual(update.public_key_der(pem), self.der)

    def test_wrong_headers_multiple_blocks_trailing_data_and_bad_base64_are_rejected(self):
        invalid = [self.pem.replace(b'PUBLIC KEY', b'RSA PUBLIC KEY'),
                   self.pem.replace(b'END PUBLIC KEY', b'END RSA PUBLIC KEY'),
                   self.pem+self.pem, self.pem+b'trailing', b'leading'+self.pem,
                   self.pem.replace(base64.b64encode(self.der), b'invalid!'),
                   self.pem.replace(base64.b64encode(self.der), b'YR=='),
                   self.pem.replace(base64.b64encode(self.der), b''),
                   self.pem.replace(base64.b64encode(self.der), b'YQ===='),
                   self.pem.replace(base64.b64encode(self.der), b' YQ==')]
        for pem in invalid:
            with self.subTest(pem=pem), self.assertRaises(ValueError):
                update.public_key_der(pem)

    def test_malformed_key_is_refused_before_apk_commands(self):
        (self.root/'keys/local.pub').write_bytes(self.pem+b'trailing')
        with patch.object(update, 'run') as run:
            with self.assertRaises(ValueError):
                update.selected_apks(self.root, self.fingerprint)
            run.assert_not_called()

    def test_fingerprint_syntax_is_validated(self):
        self.assertEqual(update.key_fingerprint(self.fingerprint.upper()), self.fingerprint)
        for invalid in ('', 'abc', 'g'*64, '../key', self.fingerprint+'\n'):
            with self.subTest(invalid=invalid), self.assertRaises(argparse.ArgumentTypeError):
                update.key_fingerprint(invalid)


if __name__ == '__main__':
    unittest.main()
