#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host tests for source correspondence, archive safety and exclusive outputs."""
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('complete_tuning', ROOT / 'developer/scripts/update-complete-tuning.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def fixture():
    tuning = (ROOT / 'developer/tuning/ov8856.yaml').read_bytes()
    original = tuning.replace(b'      gamma: 2.4\n', b'')
    entries = {}
    contents = {
        'libcamera/source.c': b'/* preserved licensed source */\n',
        'libcamera/LICENSES/GPL-2.0-only.txt': b'license text\n',
        m.REAR: original, 'release-tuning/ov02a10.yaml': b'original front\n',
        'release-tuning/uncalibrated.yaml': b'original fallback\n',
        'MODIFICATIONS.json': b'{}\n', 'COMPONENTS.json': b'{}\n', 'README.txt': b'Original source notice\n',
        **{name: b'AppleDouble fixture' for name in m.APPLEDOUBLE},
    }
    contents['SHA256SUMS'] = ''.join(m.sha(data) + '  ' + name + '\n'
                                     for name, data in sorted(contents.items()) if name not in m.METADATA).encode()
    for name, data in contents.items():
        info = tarfile.TarInfo(m.PREFIX + name)
        info.mode, info.size = 0o644, len(data)
        entries[name] = info, data
    info = tarfile.TarInfo(m.PREFIX + 'libcamera/LICENSES/GPL-2.0.txt')
    info.type, info.mode, info.linkname = tarfile.SYMTYPE, 0o777, 'GPL-2.0-only.txt'
    entries['libcamera/LICENSES/GPL-2.0.txt'] = info, None
    return entries, tuning


def archive(entries, extra=None):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w:xz', format=tarfile.PAX_FORMAT, preset=1) as result:
        for info, data in list(entries.values()) + (extra or []):
            result.addfile(info, io.BytesIO(data) if data is not None else None)
    return stream.getvalue()


def inspect(raw, legacy=True):
    return m.inspect_archive(raw, m.sha(raw), len(raw), legacy=legacy)


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.entries, self.tuning = fixture()

    def test_update_preserves_native_source_and_symlink_metadata(self):
        before = inspect(archive(self.entries))
        after = m.reissue_entries(before, self.tuning)
        checked = inspect(archive(after), legacy=False)
        self.assertEqual(m.verify_delta(before, checked, self.tuning),
                         sorted([m.REAR, 'MODIFICATIONS.json', 'README.txt', 'SHA256SUMS']))
        self.assertEqual(checked['libcamera/source.c'][1], before['libcamera/source.c'][1])
        self.assertEqual(checked['libcamera/LICENSES/GPL-2.0.txt'][0].linkname, 'GPL-2.0-only.txt')
        self.assertFalse(m.APPLEDOUBLE & set(checked))
        record = json.loads(checked['MODIFICATIONS.json'][1])[m.REAR]
        self.assertFalse(record['native_rebuild_performed'])
        self.assertEqual(record['distributed_sha256'], m.sha(self.tuning))

    def test_outer_digest_and_size_mismatch_refused(self):
        raw = archive(self.entries)
        for digest, size in [('0' * 64, len(raw)), (m.sha(raw), len(raw) - 1)]:
            with self.subTest(digest=digest, size=size), self.assertRaisesRegex(ValueError, 'outer digest/size'):
                m.inspect_archive(raw, digest, size, legacy=True)

    def test_duplicate_and_escaping_member_names_refused(self):
        duplicate = self.entries['libcamera/source.c']
        with self.assertRaisesRegex(ValueError, 'Duplicate archive'):
            inspect(archive(self.entries, [duplicate]))
        for name in ['/absolute', m.PREFIX + '../escape', m.PREFIX + 'a//b', m.PREFIX + 'a\\b',
                     m.PREFIX + 'line\nbreak', 'other-root/file']:
            entries = copy.deepcopy(self.entries)
            entries['libcamera/source.c'][0].name = name
            with self.subTest(name=name), self.assertRaises(ValueError): inspect(archive(entries))

    def test_device_hardlink_and_unsafe_symlink_refused(self):
        for kind, target in [(tarfile.CHRTYPE, ''), (tarfile.LNKTYPE, 'libcamera/source.c'),
                             (tarfile.SYMTYPE, '/etc/passwd'), (tarfile.SYMTYPE, '../source.c'),
                             (tarfile.SYMTYPE, 'missing.txt')]:
            entries = copy.deepcopy(self.entries)
            info = entries['libcamera/LICENSES/GPL-2.0.txt'][0]
            info.type, info.linkname = kind, target
            with self.subTest(kind=kind, target=target), self.assertRaises(ValueError): inspect(archive(entries))

    def test_source_permissions_or_pax_metadata_refused(self):
        for field, value in [('mode', 0o666), ('uid', 1000), ('mtime', 123),
                             ('pax_headers', {'comment': 'unreviewed metadata'})]:
            entries = copy.deepcopy(self.entries)
            setattr(entries['libcamera/source.c'][0], field, value)
            with self.subTest(field=field), self.assertRaises(ValueError): inspect(archive(entries))

    def test_internal_hash_coverage_duplicate_and_tamper_refused(self):
        original = self.entries['SHA256SUMS'][1]
        for data in [original.split(b'\n', 1)[1], original + original.splitlines(keepends=True)[0],
                     original.replace(m.sha(b'/* preserved licensed source */\n').encode(), b'0' * 64)]:
            entries = copy.deepcopy(self.entries)
            info = entries['SHA256SUMS'][0]; info.size = len(data)
            entries['SHA256SUMS'] = info, data
            with self.subTest(data=data[:40]), self.assertRaises(ValueError): inspect(archive(entries))

    def test_new_archive_requires_metadata_checksum_coverage(self):
        with self.assertRaisesRegex(ValueError, 'coverage'):
            inspect(archive(self.entries), legacy=False)

    def test_candidate_cannot_smuggle_other_tuning_or_private_text(self):
        before = inspect(archive(self.entries))
        for candidate in [self.tuning + b'# unreviewed\n', self.tuning.replace(b'gamma: 2.4', b'gamma: 3.0'),
                          self.tuning.replace(b'0.13', b'0.16'), b'-----BEGIN PRIVATE KEY-----']:
            with self.subTest(candidate=candidate[:40]), self.assertRaisesRegex(ValueError, 'one rear gamma'):
                m.reissue_entries(before, candidate)

    def test_unknown_payload_or_metadata_delta_refused(self):
        before = inspect(archive(self.entries)); after = m.reissue_entries(before, self.tuning)
        after['libcamera/source.c'] = after['libcamera/source.c'][0], b'changed code\n'
        with self.assertRaisesRegex(ValueError, 'Unexpected source payload'):
            m.verify_delta(before, after, self.tuning)
        after = m.reissue_entries(before, self.tuning)
        after['libcamera/source.c'][0].mode = 0o755
        with self.assertRaisesRegex(ValueError, 'metadata changed'):
            m.verify_delta(before, after, self.tuning)

    def test_full_build_readback_and_existing_output_protection(self):
        raw = archive(self.entries)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            source, tuning, output, report = [root / name for name in ('old.tar.xz', 'rear.yaml', 'new.tar.xz', 'report.json')]
            source.write_bytes(raw); tuning.write_bytes(self.tuning)
            with patch.object(m, 'OLD_SHA', m.sha(raw)), patch.object(m, 'OLD_BYTES', len(raw)):
                result = m.build(source, tuning, output, report)
                self.assertEqual(result['sha256'], m.sha(output.read_bytes()))
                self.assertTrue(result['native_component_bytes_and_metadata_preserved'])
                self.assertFalse(result['apk_payload_or_signature_verified_by_this_tool'])
                old_output, old_report = output.read_bytes(), report.read_bytes()
                with self.assertRaisesRegex(ValueError, 'already exists'): m.build(source, tuning, output, report)
                self.assertEqual(output.read_bytes(), old_output); self.assertEqual(report.read_bytes(), old_report)
                self.assertEqual(source.read_bytes(), raw)

    def test_existing_report_or_output_symlink_refused_before_creating_archive(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve(); output, report = root / 'new.tar.xz', root / 'report.json'
            report.write_bytes(b'keep')
            with self.assertRaisesRegex(ValueError, 'already exists'):
                m.build(root / 'missing', root / 'missing', output, report)
            self.assertFalse(output.exists()); self.assertEqual(report.read_bytes(), b'keep')
            report.unlink(); output.symlink_to(root / 'missing-target')
            with self.assertRaisesRegex(ValueError, 'already exists'):
                m.build(root / 'missing', root / 'missing', output, report)
            self.assertTrue(output.is_symlink()); self.assertFalse(report.exists())


if __name__ == '__main__':
    unittest.main()
