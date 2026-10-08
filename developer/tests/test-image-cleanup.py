#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Duet camera project contributors
"""Run the image builder with simulated mounts; no device commands are executed."""
import collections
import contextlib
import hashlib
import io
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/apply-external-overlay-to-image.py'


class ImageFixture:
    def __init__(self, directory, *, failures=None, build_failure=None, detach_failure=False):
        self.directory = Path(directory)
        self.source = self.directory / 'source.img'
        self.source.write_bytes(b'offline source image')
        self.prepared = self.directory / 'prepared'
        self.overlay = self.prepared / 'overlay'
        self.output = self.directory / 'new.img'
        self.root = Path(str(self.output) + '.build')
        self.loop = '/dev/loop999'
        self.active = set()
        self.unmount_calls = collections.Counter()
        self.commands = []
        self.detached = False
        self.failures = dict(failures or {})
        self.build_failure = build_failure
        self.detach_failure = detach_failure
        self.stdout = io.StringIO()
        self.stderr = io.StringIO()
        self.error = None
        links = self.overlay / 'etc/systemd/system/multi-user.target.wants'
        links.mkdir(parents=True)
        for name in ('duet-camera-activate.service', 'duet-camera-scp-init.service'):
            (links / name).symlink_to('/usr/lib/systemd/system/' + name)
        config = self.overlay / 'etc/example.conf'
        config.write_bytes(b'fixture config\n')
        (self.prepared / 'manifest.json').write_text(json.dumps({
            'files_sha256': {'etc/example.conf': hashlib.sha256(config.read_bytes()).hexdigest()},
            'root_uuid': 'fixture-root', 'boot_uuid': 'fixture-boot',
        }))

    def command(self, args, **kwargs):
        args = list(map(str, args))
        self.commands.append(args)
        stdout = ''
        if args[:3] == ['losetup', '--json', '--list']:
            stdout = json.dumps({'loopdevices': []})
        elif args[:4] == ['losetup', '--find', '--show', '--partscan']:
            stdout = self.loop + '\n'
        elif args[:2] == ['losetup', '-d']:
            if self.active:
                raise AssertionError('Builder tried to detach a mounted loop')
            if self.detach_failure:
                raise subprocess.CalledProcessError(1, args)
            self.detached = True
        elif args[0] == 'sfdisk':
            stdout = json.dumps({'partitiontable': {
                'label': 'gpt', 'sectorsize': 512, 'partitions': [
                    {'node': self.loop + 'p1', 'start': 8192, 'size': 1024,
                     'type': 'fe3a2a5d-4f32-41a7-b725-accc3285a309'},
                    {'node': self.loop + 'p2'}, {'node': self.loop + 'p3'},
                ],
            }})
        elif args[0] == 'blkid':
            stdout = 'fixture-boot' if args[-1].endswith('p2') else 'fixture-root'
        elif args[0] == 'mount':
            point = Path(args[-1])
            point.mkdir(parents=True, exist_ok=True)
            self.active.add(point)
            if point == self.root:
                (point / 'boot').mkdir()
            if point == self.root / 'boot':
                (point / 'vmlinuz.kpart').write_bytes(b'fixture kernel partition')
        elif args[0] == 'umount':
            point = Path(args[1])
            name = str(point.relative_to(self.root))
            self.unmount_calls[name] += 1
            if point not in self.active:
                raise AssertionError('Builder retried an already unmounted path: ' + name)
            remaining = self.failures.get(name, 0)
            if remaining:
                self.failures[name] = remaining - 1
                raise subprocess.CalledProcessError(1, args)
            if any(point in other.parents for other in self.active):
                raise subprocess.CalledProcessError(32, args)
            self.active.remove(point)
        elif args[:2] == ['cp', '-a']:
            shutil.copytree(self.overlay, self.root, dirs_exist_ok=True, symlinks=True)
        elif args[0] == 'mknod':
            Path(args[1]).touch()
        elif args[0] in ('chroot', 'e2fsck'):
            if self.build_failure and self.build_failure in args:
                raise subprocess.CalledProcessError(7, args)
        elif args != ['sync']:
            raise AssertionError('Unexpected simulated command: ' + repr(args))
        return subprocess.CompletedProcess(args, 0, stdout=stdout)

    def execute(self):
        original_read_text = Path.read_text
        backing_file = Path('/sys/block/loop999/loop/backing_file')

        def read_text(path, *args, **kwargs):
            if path == backing_file:
                return str(self.output) + '\n'
            return original_read_text(path, *args, **kwargs)

        argv = [str(SCRIPT), '--image', str(self.source), '--prepared', str(self.prepared),
                '--output', str(self.output)]
        with mock.patch.object(sys, 'argv', argv), \
                mock.patch('subprocess.run', side_effect=self.command), \
                mock.patch('os.geteuid', return_value=0), \
                mock.patch.object(Path, 'read_text', read_text), \
                contextlib.redirect_stdout(self.stdout), contextlib.redirect_stderr(self.stderr):
            try:
                runpy.run_path(str(SCRIPT), run_name='__main__')
            except Exception as error:
                self.error = error


class CleanupTests(unittest.TestCase):
    def fixture(self, **kwargs):
        temporary = tempfile.TemporaryDirectory(prefix='duet-image-cleanup-')
        self.addCleanup(temporary.cleanup)
        fixture = ImageFixture(temporary.name, **kwargs)
        fixture.execute()
        self.assertEqual(fixture.source.read_bytes(), b'offline source image')
        return fixture

    def assert_no_success_evidence(self, fixture):
        self.assertFalse(Path(str(fixture.output) + '.validation.json').exists())
        self.assertNotIn('"PASS"', fixture.stdout.getvalue())

    def test_success_unmounts_each_once_before_detaching(self):
        fixture = self.fixture()
        self.assertIsNone(fixture.error)
        self.assertTrue(fixture.detached)
        self.assertFalse(fixture.active)
        self.assertEqual(fixture.unmount_calls, {'.': 1, 'boot': 1, 'proc': 1, 'sys': 1, 'dev': 1})
        self.assertTrue(Path(str(fixture.output) + '.validation.json').exists())

    def test_transient_unmount_failure_does_not_retry_successes(self):
        fixture = self.fixture(failures={'sys': 1})
        self.assertIsInstance(fixture.error, RuntimeError)
        self.assertTrue(fixture.detached)
        self.assertFalse(fixture.active)
        self.assertEqual(fixture.unmount_calls, {'.': 2, 'boot': 1, 'proc': 1, 'sys': 2, 'dev': 1})
        self.assert_no_success_evidence(fixture)

    def test_persistent_failure_cleans_independent_mounts_and_keeps_loop(self):
        fixture = self.fixture(failures={'sys': 100})
        self.assertIsInstance(fixture.error, RuntimeError)
        self.assertFalse(fixture.detached)
        self.assertEqual(fixture.active, {fixture.root, fixture.root / 'sys'})
        self.assertEqual(fixture.unmount_calls, {'.': 2, 'boot': 1, 'proc': 1, 'sys': 2, 'dev': 1})
        self.assertIn('Loop retained while mounts remain', fixture.stderr.getvalue())
        self.assertIn(str(fixture.root / 'sys'), fixture.stderr.getvalue())
        self.assert_no_success_evidence(fixture)

    def test_original_build_error_survives_cleanup_failure(self):
        fixture = self.fixture(build_failure='mkinitfs', failures={'sys': 100})
        self.assertIsInstance(fixture.error, subprocess.CalledProcessError)
        self.assertIn('mkinitfs', fixture.error.cmd)
        self.assertEqual(fixture.error.returncode, 7)
        self.assertEqual(fixture.active, {fixture.root, fixture.root / 'sys'})
        self.assertIn('Image cleanup failed', fixture.stderr.getvalue())
        self.assertIn('Loop retained while mounts remain', fixture.stderr.getvalue())
        self.assert_no_success_evidence(fixture)

    def test_detach_failure_is_reported_without_success_evidence(self):
        fixture = self.fixture(detach_failure=True)
        self.assertIsInstance(fixture.error, RuntimeError)
        self.assertIn('losetup', str(fixture.error))
        self.assertFalse(fixture.active)
        self.assertFalse(fixture.detached)
        self.assert_no_success_evidence(fixture)

    def test_original_build_error_survives_detach_failure(self):
        fixture = self.fixture(build_failure='mkinitfs', detach_failure=True)
        self.assertIsInstance(fixture.error, subprocess.CalledProcessError)
        self.assertIn('mkinitfs', fixture.error.cmd)
        self.assertIn('losetup', fixture.stderr.getvalue())
        self.assertFalse(fixture.active)
        self.assert_no_success_evidence(fixture)

    def test_filesystem_error_still_detaches_already_unmounted_loop(self):
        fixture = self.fixture(build_failure='e2fsck')
        self.assertIsInstance(fixture.error, subprocess.CalledProcessError)
        self.assertIn('e2fsck', fixture.error.cmd)
        self.assertTrue(fixture.detached)
        self.assertFalse(fixture.active)
        self.assertTrue(all(count == 1 for count in fixture.unmount_calls.values()))
        self.assert_no_success_evidence(fixture)


if __name__ == '__main__':
    unittest.main()
