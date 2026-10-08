#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host fixtures run the real packager; only native commands/identity are mocked.

No device, real package installation, signing key or network is used. Checks
remain active under python -O. Native signing/extraction is separately required.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/package-rear-brightness.py'
spec = importlib.util.spec_from_file_location('rear_packager', SCRIPT)
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
sha = lambda data: hashlib.sha256(data).hexdigest()
encoded = lambda value: (json.dumps(value, sort_keys=True) + '\n').encode()
PUBLIC = b'Fixture public key; no real key material'
DER = b'Fixture public identity'
ORIGINAL = b'algorithms:\n  - Adjust:\n'
CANDIDATE = ORIGINAL + b'      gamma: 2.4\n'
STAMP = 1700000000


class PackageFixture(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.kit = self.root / 'kit'; (self.kit / 'docs').mkdir(parents=True)
        (self.kit / 'tuning').mkdir(); (self.kit / 'tuning/ov8856.yaml').write_bytes(CANDIDATE)
        self.source = self.root / 'accepted'; self.repo = self.source / 'repo/aarch64'
        self.repo.mkdir(parents=True)
        self.key = self.root / 'key'; self.key.write_bytes(b'fixture placeholder'); self.key.chmod(0o600)
        self.public = self.root / 'fixture.pub'; self.public.write_bytes(PUBLIC)
        self.output = self.root / 'output'
        self.calls = []; self.fault = None; self.documents = {}; self.payloads = {}; self.manifest = {}
        old = json.loads((SCRIPT.parents[1] / 'docs/PRE_REAR_BRIGHTNESS_PACKAGES.json').read_text())['packages']
        for name, details in old.items():
            info = {k: v for k, v in details.items() if k != 'sha256'}
            files = {'usr/share/notice.txt': {'data': 'notice', 'mode': 0o644},
                     'usr/lib/fixture.so.1': {'data': '\x7fELFpreserved', 'mode': 0o755},
                     'usr/lib/fixture.so': {'target': 'fixture.so.1', 'mode': 0o777}}
            if info['name'] == 'libcamera-ipa':
                files[m.TUNING] = {'data': ORIGINAL.decode(), 'mode': 0o644}
            document = self.document(info, files)
            data = encoded({'document': document, 'files': files})
            (self.repo / name).write_bytes(data)
            self.documents[name] = document
            self.manifest[name] = dict(info, sha256=sha(data))
            self.payloads[name] = {p: sha(x['data'].encode()) for p, x in files.items() if 'data' in x}
        (self.repo / 'packages.adb').write_bytes(b'fixture original index')
        self.refresh()

    def refresh(self):
        (self.source / 'packages.json').write_bytes(encoded(self.manifest))
        (self.source / 'payload-hashes.json').write_bytes(encoded(self.payloads))
        self.history = {'schema': 1, 'source_release': 'v1.0.1', 'packages': self.manifest,
                        'packages_sha256': sha((self.source / 'packages.json').read_bytes()),
                        'payload_sha256': sha((self.source / 'payload-hashes.json').read_bytes()),
                        'index_sha256': sha((self.repo / 'packages.adb').read_bytes()),
                        'public_key_name': self.public.name, 'public_key_sha256': sha(PUBLIC),
                        'public_key_spki_sha256': sha(DER)}
        (self.kit / 'docs/PRE_REAR_BRIGHTNESS_PACKAGES.json').write_bytes(encoded(self.history))

    def document(self, info, files):
        directories = {''}
        for name in files:
            directories.update(str(p) for p in Path(name).parents if str(p) != '.')
        paths = []
        for name in sorted(directories):
            node = {'acl': {'mode': 0o755, 'user': 'root', 'group': 'root'}}
            if name: node['name'] = name
            items = []
            for filename, value in sorted(files.items()):
                if str(Path(filename).parent) != (name or '.'): continue
                item = {'name': Path(filename).name,
                        'acl': {'mode': value['mode'], 'user': 'root', 'group': 'root'},
                        'mtime': value.get('mtime', STAMP)}
                if 'data' in value:
                    data = value['data'].encode(); item.update(size=len(data), hash=sha(data))
                else:
                    target = stat.S_IFLNK.to_bytes(2, 'little') + value['target'].encode()
                    item.update(target=target.hex(), size=len(value['target'].encode()))
                items.append(item)
            if items: node['files'] = items
            paths.append(node)
        identity = {k: sorted(v.split()) if k in ('depends', 'provides') else v for k, v in info.items()}
        identity.update(hashes='a' * 40, **{'installed-size': sum(len(v.get('data', '')) for v in files.values())})
        return {'info': identity, 'paths': paths}

    def command(self, argv, **kwargs):
        argv = list(map(str, argv)); self.calls.append(argv)
        stdout = b''
        if argv[:2] == ['apk', '--version']:
            return subprocess.CompletedProcess(argv, 0, 'apk-tools 3.0.8, compiled for aarch64.\n')
        if argv[0] == 'openssl':
            return subprocess.CompletedProcess(argv, 0, DER if self.fault != 'key' else b'wrong key')
        op = argv[1]
        if op == 'verify':
            if self.fault == 'signature': raise subprocess.CalledProcessError(1, argv)
        elif op == 'adbdump':
            path = Path(argv[-1])
            data = json.loads(path.read_bytes())
            result = data.get('document', data)
            if self.fault == 'output-metadata' and self.output in path.parents and path.suffix == '.apk':
                result['info']['depends'].append('unrequested=1')
            stdout = encoded(result)
        elif op == 'extract':
            dest = Path(argv[argv.index('--destination') + 1]); source = Path(argv[-1])
            data = json.loads(source.read_bytes()); dest.chmod(0o755)
            for name, entry in data['files'].items():
                target = dest / name; target.parent.mkdir(parents=True, exist_ok=True)
                if 'target' in entry:
                    target.symlink_to(entry['target'])
                    if hasattr(os, 'lchmod'): os.lchmod(target, entry['mode'])
                    # Reproduce native apk 3.0.8: it does not restore link mtime.
                    os.utime(target, ns=(STAMP * 10**9, (STAMP + 777) * 10**9), follow_symlinks=False)
                else:
                    target.write_bytes(entry['data'].encode()); target.chmod(entry['mode'])
                    os.utime(target, (entry.get('mtime', STAMP), entry.get('mtime', STAMP)))
            if source.name.startswith('libcamera-ipa-'):
                target = dest / m.TUNING
                if self.fault == 'extra': (dest / 'unlisted').write_bytes(b'extra')
                if self.fault == 'hardlink':
                    target.unlink(); os.link(self.key, target)
                if self.fault == 'symlink':
                    target.unlink(); target.symlink_to(self.key)
                if self.fault == 'parent-symlink':
                    shutil.rmtree(target.parent); target.parent.symlink_to(self.root)
                if self.fault == 'output-bytes' and 'verified' in dest.parts:
                    (dest / 'usr/lib/fixture.so.1').write_bytes(b'changed ELF')
                if self.fault == 'output-mode' and 'verified' in dest.parts: target.chmod(0o600)
                if self.fault == 'regular-mtime': os.utime(target, (STAMP + 1, STAMP + 1))
                if self.fault == 'link-target':
                    link = dest / 'usr/lib/fixture.so'; link.unlink(); link.symlink_to('different.so')
                    if hasattr(os, 'lchmod'): os.lchmod(link, 0o777)
        elif op == 'mkpkg':
            stage = Path(argv[argv.index('--files') + 1]); dest = Path(argv[argv.index('--output') + 1])
            info = dict(argv[i + 1].split(':', 1) for i, x in enumerate(argv) if x == '--info')
            files = {}
            for name, item in m.tree(stage).items():
                if item['type'] == 'file': files[name] = {'data': (stage / name).read_bytes().decode(), 'mode': item['mode'],
                                                        'mtime': item['mtime_ns'] // 10**9}
                elif item['type'] == 'symlink': files[name] = {'target': item['target'], 'mode': item['mode'],
                                                             'mtime': item['mtime_ns'] // 10**9}
            dest.write_bytes(encoded({'document': self.document(info, files), 'files': files}))
        elif op == 'mkndx':
            dest = Path(argv[argv.index('--output') + 1]); infos = []
            for path in map(Path, [s for s in argv if s.endswith('.apk')]):
                data = path.read_bytes(); info = json.loads(data)['document']['info']
                info.update(hashes=sha(data), **{'file-size': len(data)}); infos.append(info)
            if self.fault == 'index' and '--sign-key' in argv: infos[0]['hashes'] = 'b' * 64
            dest.write_bytes(encoded({'packages': infos}))
        else: raise RuntimeError('Unexpected fixture command: ' + op)
        return subprocess.CompletedProcess(argv, 0, stdout)

    def execute(self):
        args = argparse.Namespace(accepted_repository=self.source, output=self.output,
                                  sign_key=self.key, public_key=self.public, sign_key_owner_uid=os.getuid())
        with patch.object(m, '__file__', str(self.kit / 'scripts/package.py')), \
                patch.object(m, 'HISTORY_SHA256', sha(encoded(self.history))), \
                patch.object(m, 'OLD_TUNING_SHA256', sha(ORIGINAL)), \
                patch.object(m, 'NEW_TUNING_SHA256', sha(CANDIDATE)), \
                patch.object(m, 'ROOT_UID', os.getuid()), patch.object(m, 'ROOT_GID', os.getgid()), \
                patch.object(m.os, 'geteuid', return_value=0), patch.object(m, 'run', self.command):
            return m.package(args)

    def test_success_changes_only_rear_tuning_and_preserves_five_apks(self):
        result = self.execute()
        self.assertEqual((result['reissued_packages'], result['byte_reused_packages']), (6, 5))
        records = json.loads((self.output / 'correspondence.json').read_text())
        changed = [x for x in records.values() if x['changed_regular_files']]
        self.assertEqual(len(changed), 1)
        self.assertEqual(changed[0]['changed_regular_files'], [m.TUNING])
        self.assertEqual(changed[0]['actual_tuning_sha256'], sha(CANDIDATE))
        for name, original in self.manifest.items():
            if name not in m.CHANGES:
                self.assertEqual((self.output / 'repo/aarch64' / name).read_bytes(), (self.repo / name).read_bytes())
        self.assertEqual(sum(c[:2] == ['apk', 'mkpkg'] for c in self.calls), 6)
        self.assertEqual(sum(c[:2] == ['apk', 'extract'] for c in self.calls), 22)
        self.assertFalse(any('add' in c or '--allow-untrusted' in c for c in self.calls))

    def test_input_hash_failure_prevents_output(self):
        next(self.repo.glob('*.apk')).write_bytes(b'wrong')
        with self.assertRaises(ValueError): self.execute()
        self.assertFalse(self.output.exists())

    def test_unknown_repository_member_prevents_output(self):
        (self.repo / 'extra.apk').write_bytes(b'unknown')
        with self.assertRaises(ValueError): self.execute()
        self.assertFalse(self.output.exists())

    def test_bad_candidate_prevents_output(self):
        (self.kit / 'tuning/ov8856.yaml').write_bytes(CANDIDATE + b'# unexpected\n')
        with self.assertRaises(ValueError): self.execute()
        self.assertFalse(self.output.exists())

    def test_unsafe_payload_paths_prevent_extraction(self):
        filename = next(iter(self.payloads))
        for unsafe in ('/tmp/outside', '../outside', 'x//y', 'x/../y', 'x\\y', 'x\0y'):
            with self.subTest(path=unsafe):
                self.payloads[filename] = {unsafe: 'a' * 64}; self.refresh()
                with self.assertRaises(ValueError): self.execute()
                self.assertFalse(self.output.exists())
        self.assertFalse(any(c[:2] == ['apk', 'extract'] for c in self.calls))

    def test_duplicate_json_refused(self):
        with self.assertRaises(ValueError): m.decode(b'{"a":1,"a":2}')

    def test_signature_failure_prevents_output(self):
        self.fault = 'signature'
        with self.assertRaises(subprocess.CalledProcessError): self.execute()
        self.assertFalse(self.output.exists())

    def test_wrong_key_prevents_output(self):
        self.fault = 'key'
        with self.assertRaises(ValueError): self.execute()
        self.assertFalse(self.output.exists())

    def test_nonprivate_key_rejected_without_ownership_change(self):
        self.key.chmod(0o644)
        with self.assertRaises(ValueError): self.execute()
        self.assertEqual(stat.S_IMODE(self.key.stat().st_mode), 0o644)

    def test_existing_output_and_symlink_are_not_replaced(self):
        self.output.mkdir(); (self.output / 'keep').write_bytes(b'keep')
        with self.assertRaises(ValueError): self.execute()
        self.assertEqual((self.output / 'keep').read_bytes(), b'keep')
        shutil.rmtree(self.output); self.output.symlink_to(self.root)
        with self.assertRaises(ValueError): self.execute()
        self.assertTrue(self.output.is_symlink())

    def test_extraction_faults_stop_and_retain_failed_output(self):
        for fault in ('extra', 'hardlink', 'symlink', 'parent-symlink', 'output-bytes', 'output-mode',
                      'output-metadata', 'index', 'regular-mtime', 'link-target'):
            with self.subTest(fault=fault):
                self.output = self.root / ('output-' + fault); self.fault = fault
                with self.assertRaises((ValueError, OSError)): self.execute()
                self.assertTrue(self.output.exists())
                self.assertFalse((self.output / 'validation.json').exists())
                self.assertEqual(self.key.read_bytes(), b'fixture placeholder')
                shutil.rmtree(self.output)  # Dispose only this test's intentional hardlink.

    def test_native_symlink_mtime_is_restored_without_touching_target(self):
        self.execute()
        for directory in ('stages', 'verified'):
            for stage in (self.output / directory).iterdir():
                if not stage.is_dir(): continue
                link = stage / 'usr/lib/fixture.so'
                target = stage / 'usr/lib/fixture.so.1'
                self.assertTrue(link.is_symlink())
                self.assertEqual(link.lstat().st_mtime_ns, STAMP * 10**9)
                self.assertEqual(target.stat().st_mtime_ns, STAMP * 10**9)
                self.assertEqual(target.read_bytes(), b'\x7fELFpreserved')

    def test_no_timestamp_writes_before_invalid_link_target_is_rejected(self):
        self.fault = 'link-target'
        original = os.utime
        calls = []
        def record(*args, **kwargs):
            calls.append((args, kwargs)); return original(*args, **kwargs)
        with patch.object(m.os, 'utime', side_effect=record), self.assertRaises(ValueError): self.execute()
        # Extraction itself sets test mtimes; packager must never repair a link
        # with the wrong target. Earlier verified packages may be normalized.
        wrong_stage = self.output / 'stages/libcamera-ipa-0.7.2-r102/usr/lib/fixture.so'
        repairs = [k for a, k in calls if a[0] == wrong_stage and k.get('ns', (0, 0))[1] == STAMP * 10**9]
        self.assertEqual(repairs, [])

    def test_adb_rejects_paths_scripts_and_duplicate_nodes(self):
        original = next(iter(self.documents.values()))
        for mutation in ('traversal', 'duplicate', 'scripts'):
            document = copy.deepcopy(original)
            if mutation == 'traversal': document['paths'][1]['name'] = '../outside'
            elif mutation == 'duplicate': document['paths'].append(document['paths'][0])
            else: document['scripts'] = {'post-install': 'anything'}
            with self.subTest(mutation=mutation), self.assertRaises(ValueError): m.adb_paths(document)

    def test_output_metadata_change_rejected(self):
        before = next(iter(self.documents.values())); after = copy.deepcopy(before)
        after['info']['license'] = 'unknown'
        expected = next(iter(self.manifest.values()))
        with self.assertRaises(ValueError): m.compare_documents(before, after, expected, set())


if __name__ == '__main__':
    unittest.main()
