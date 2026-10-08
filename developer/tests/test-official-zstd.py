#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Official supplement trust and temporary decoder tests; no real APK runs."""
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1]/'scripts/apply-update.py'
spec = importlib.util.spec_from_file_location('official_zstd_under_test', SCRIPT)
update = importlib.util.module_from_spec(spec)
spec.loader.exec_module(update)


class OfficialZstd(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='duet-official-test-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.directory = self.root/'packages'; self.supplement = self.directory/'official-zstd'
        self.supplement.mkdir(parents=True)
        self.trusted = self.root/'trusted'; self.trusted.mkdir()
        self.apk = self.supplement/'zstd-1.5.7-r2.apk'; self.apk.write_bytes(b'official APK fixture')
        self.index = self.supplement/'APKINDEX.tar.gz'; self.index.write_bytes(b'official signed index fixture')
        self.key = self.trusted/update.OFFICIAL_ZSTD['key_name']; self.key.write_bytes(b'official key fixture')
        (self.trusted/'camera-author.pub').write_bytes(b'author key must not authenticate official supplement')
        self.policy = dict(update.OFFICIAL_ZSTD, apk_sha256=update.sha(self.apk),
                           index_sha256=update.sha(self.index), key_sha256=update.sha(self.key))
        self.addCleanup(patch.stopall)
        patch.object(update, 'OFFICIAL_ZSTD', self.policy).start()
        self.before = {'base': '1-r0', 'zstd-libs': '1.5.7-r2'}
        self.calls = []
        self.run = patch.object(update, 'run', side_effect=self.command).start()
        self.plan = patch.object(update, 'sandbox', side_effect=self.sandbox).start()

    def command(self, args, **kwargs):
        args = list(map(str, args)); self.calls.append(args)
        if args[:2] == ['apk', 'verify']:
            self.assertEqual(sorted(p.name for p in Path(args[3]).iterdir()), [self.key.name])
        elif args[:2] == ['apk', 'extract']:
            self.assertNotIn('--allow-untrusted', args)
            keys = Path(args[args.index('--keys-dir')+1])
            self.assertEqual(sorted(p.name for p in keys.iterdir()), [self.key.name])
            target = Path(args[args.index('--destination')+1])/'usr/bin/zstd'
            target.parent.mkdir(parents=True); target.write_text('fixture decoder')
        else:
            raise AssertionError('Unexpected command: '+repr(args))
        return subprocess.CompletedProcess(args, 0, stdout='')

    def sandbox(self, specs, keys, repositories=()):
        self.assertEqual(specs, ['zstd=1.5.7-r2'])
        self.assertEqual(repositories, [self.supplement])
        self.assertEqual(sorted(p.name for p in keys.iterdir()), [self.key.name])
        return dict(self.before, zstd='1.5.7-r2'), 'fixture signed-index transaction'

    def select(self):
        return update.selected_zstd(self.directory, self.before, self.trusted)

    def test_official_only_key_and_index_validate_before_selection(self):
        self.assertEqual(self.select(), [self.apk])
        self.run.assert_called_once(); self.plan.assert_called_once()

    def test_existing_zstd_is_never_updated_or_reverified(self):
        self.before['zstd'] = 'different-installed-version'
        self.apk.unlink(); self.key.unlink()
        self.assertEqual(self.select(), [])
        self.run.assert_not_called(); self.plan.assert_not_called()

    def test_digest_and_existing_os_key_fail_closed(self):
        for path in (self.apk, self.index, self.key):
            with self.subTest(path=path.name):
                original = path.read_bytes(); path.write_bytes(b'changed')
                with self.assertRaises(ValueError): self.select()
                self.run.assert_not_called(); self.plan.assert_not_called()
                path.write_bytes(original)

    def test_missing_pin_or_symlink_cannot_add_a_package(self):
        self.policy['apk_sha256'] = None
        with self.assertRaisesRegex(ValueError, 'not been pinned'): self.select()
        self.policy['apk_sha256'] = update.sha(self.apk)
        moved = self.root/self.apk.name; self.apk.rename(moved); self.apk.symlink_to(moved)
        with self.assertRaisesRegex(ValueError, 'regular official'): self.select()
        self.run.assert_not_called()

    def test_package_signature_failure_does_not_reach_index_transaction(self):
        self.run.side_effect = subprocess.CalledProcessError(1, ['apk', 'verify'])
        with self.assertRaises(subprocess.CalledProcessError): self.select()
        self.plan.assert_not_called()

    def test_signed_index_or_dependency_change_failure_is_not_ignored(self):
        self.plan.side_effect = subprocess.CalledProcessError(1, ['apk', 'add'])
        with self.assertRaises(subprocess.CalledProcessError): self.select()
        self.plan.side_effect = None
        for after in (dict(self.before, zstd='wrong-version'),
                      dict(self.before, zstd='1.5.7-r2', base='2-r0'),
                      dict(self.before, zstd='1.5.7-r2', extra='1-r0')):
            with self.subTest(after=after):
                self.plan.return_value = after, ''
                with self.assertRaisesRegex(ValueError, 'other packages'): self.select()

    def test_existing_decoder_needs_no_supplement(self):
        with patch.object(update.os, 'geteuid', return_value=0), \
                patch.object(update.shutil, 'which', return_value='/usr/bin/zstd'):
            with update.checker_zstd(self.directory, self.trusted) as env:
                self.assertIsNone(env)
        self.run.assert_not_called(); self.plan.assert_not_called()

    def test_nonroot_bootstrap_is_rejected_before_commands(self):
        with patch.object(update.os, 'geteuid', return_value=1000):
            with self.assertRaisesRegex(ValueError, 'sudo'):
                with update.checker_zstd(self.directory, self.trusted): pass
        self.run.assert_not_called(); self.plan.assert_not_called()

    def test_temporary_decoder_does_not_change_environment_and_is_removed(self):
        original_path = os.environ.get('PATH')
        def output(args):
            if list(args) == ['apk', '--version']: return 'apk-tools 3.0.8'
            self.assertEqual(args[-1], '--version')
            self.assertTrue(Path(args[0]).is_file())
            return '*** Zstandard CLI (64-bit) v1.5.7, by Yann Collet ***'
        with patch.object(update.os, 'geteuid', return_value=0), \
                patch.object(update.shutil, 'which', return_value=None), \
                patch.object(update, 'packages', return_value=self.before), \
                patch.object(update, 'output', side_effect=output):
            with update.checker_zstd(self.directory, self.trusted) as env:
                executable = Path(env['PATH'].split(os.pathsep)[0])/'zstd'
                self.assertTrue(executable.is_file())
                self.assertEqual(os.environ.get('PATH'), original_path)
            self.assertFalse(executable.exists())
        self.assertEqual(os.environ.get('PATH'), original_path)
        self.assertEqual([args[:2] for args in self.calls], [['apk', 'verify'], ['apk', 'extract']])

    def test_checker_environment_is_passed_only_to_compatibility_check(self):
        env = {'PATH': '/temporary/verified-decoder:/usr/bin'}
        self.run.side_effect = None
        self.run.return_value = subprocess.CompletedProcess([], 0, stdout='{"compatible":true}')
        with patch.object(update, 'output', return_value='apk-tools 3.0.8') as ordinary:
            update.compatibility_gate(env)
        self.assertEqual(self.run.call_args.kwargs['env'], env)
        self.assertEqual(self.run.call_args.args[0][0], 'python3')
        ordinary.assert_called_once_with(['apk', '--version'])


if __name__ == '__main__':
    unittest.main()
