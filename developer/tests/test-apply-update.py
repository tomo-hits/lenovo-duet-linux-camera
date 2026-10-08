#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise update recovery with disposable fixtures, never the host root.

Package and capture fixtures describe interrupted transactions and real
diagnostic fields. Config backup/restore and state-file writes use actual
temporary files; only the root path and system commands are substituted.
These tests remain effective under python -O.
"""
import importlib.util
import base64
import contextlib
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/apply-update.py'
spec = importlib.util.spec_from_file_location('apply_update_under_test', SCRIPT)
update = importlib.util.module_from_spec(spec)
spec.loader.exec_module(update)


class CaptureState(unittest.TestCase):
    fresh = ('schema=3 capture_error=0 input_error=0 inputs_idle=0\n'
             'epoch=0 sof_count=0 done_count=0 finalized=0 pending=0 refs=0\n')
    stopped = ('schema=3 capture_error=0 input_error=0 inputs_idle=1\n'
               'epoch=7 sof_count=194 done_count=194 finalized=1 pending=0 refs=0\n')

    def test_never_started_does_not_require_a_finished_capture(self):
        self.assertTrue(update.stopped_capture(self.fresh))

    def test_probe_and_post_sleep_unpublished_idle(self):
        idle = self.fresh.replace('inputs_idle=0', 'inputs_idle=1') + 'unpublished_idle=1'
        self.assertTrue(update.stopped_capture(idle))
        for field in ('capture_error', 'input_error', 'pending', 'refs', 'sof_count', 'done_count'):
            self.assertFalse(update.stopped_capture(idle.replace(field+'=0', field+'=1')))
        self.assertFalse(update.stopped_capture(idle.replace('inputs_idle=1','inputs_idle=0').replace('epoch=0','epoch=1')))

    def test_normally_finished_capture(self):
        self.assertTrue(update.stopped_capture(self.stopped))

    def test_started_but_unfinalized_is_refused(self):
        self.assertFalse(update.stopped_capture(self.stopped.replace('finalized=1', 'finalized=0')))
        # An attempted stream with no completed frame is not a fresh device.
        self.assertFalse(update.stopped_capture(self.fresh.replace('epoch=0', 'epoch=7')))

    def test_errors_or_held_work_are_refused_even_before_first_frame(self):
        for text in (self.fresh, self.stopped):
            for field in ('capture_error', 'input_error', 'pending', 'refs'):
                with self.subTest(fixture=text, field=field):
                    self.assertFalse(update.stopped_capture(text.replace(field+'=0', field+'=1')))
        self.assertFalse(update.stopped_capture(self.stopped.replace('inputs_idle=1', 'inputs_idle=0')))

    def test_incomplete_diagnostics_fail_closed(self):
        self.assertFalse(update.stopped_capture(''))
        for field in ('capture_error=0', 'input_error=0', 'pending=0', 'refs=0'):
            with self.subTest(field=field):
                self.assertFalse(update.stopped_capture(self.fresh.replace(field, '')))


class PackageRecovery(unittest.TestCase):
    before = {'base': '1-r0', 'libcamera': '99-r0', 'pipewire': '1-r0', 'old-dep': '2-r0'}
    after = {'base': '1-r0', 'libcamera': '7-r1', 'pipewire': '1-r2', 'camera': '2-r0'}

    def recoverable(self, actual, status='APPLYING'):
        return update.recoverable_packages(actual, self.before, self.after, status)

    def test_interrupted_old_new_and_partial_delta_are_recoverable(self):
        fixtures = [self.before, self.after,
                    {'base': '1-r0', 'libcamera': '7-r1', 'pipewire': '1-r0', 'old-dep': '2-r0'},
                    {'base': '1-r0', 'libcamera': '99-r0', 'pipewire': '1-r2', 'camera': '2-r0'},
                    {'base': '1-r0', 'libcamera': '7-r1', 'pipewire': '1-r0'}]
        for status in ('APPLYING', 'RESTORING'):
            for actual in fixtures:
                with self.subTest(status=status, actual=actual):
                    self.assertTrue(self.recoverable(actual, status))

    def test_unrelated_install_upgrade_or_removal_is_refused(self):
        fixtures = [dict(self.after, unrelated='1-r0'), dict(self.after, base='2-r0'),
                    {n: v for n, v in self.after.items() if n != 'base'}]
        for actual in fixtures:
            with self.subTest(actual=actual):
                self.assertFalse(self.recoverable(actual))

    def test_unplanned_version_and_missing_replaced_package_are_refused(self):
        self.assertFalse(self.recoverable(dict(self.after, libcamera='7-r9')))
        self.assertFalse(self.recoverable({n: v for n, v in self.after.items() if n != 'libcamera'}))
        self.assertFalse(self.recoverable(dict(self.after, **{'old-dep': '3-r0'})))

    def test_completed_apply_requires_exact_after_set(self):
        self.assertTrue(self.recoverable(self.after, 'APPLIED'))
        self.assertFalse(self.recoverable(self.before, 'APPLIED'))
        self.assertFalse(self.recoverable(dict(self.after, **{'old-dep': '2-r0'}), 'APPLIED'))


class TemporaryFiles(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='duet-update-test-')
        self.addCleanup(temporary.cleanup)
        # Resolve macOS /var's symlink so parent-symlink checks remain real.
        self.root = Path(temporary.name).resolve()

    def write(self, name, data=b'original', mode=0o600):
        p = self.root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        p.chmod(mode)
        return p

    def rooted_path(self, name):
        # Config operations only request absolute '/'. Refuse any future
        # absolute path expansion rather than risk touching the host root.
        p = Path(name)
        if p.is_absolute():
            if p != Path('/'):
                raise AssertionError('Unexpected host path: '+str(p))
            return self.root
        return p


class CachedPackageBackup(TemporaryFiles):
    def setUp(self):
        super().setUp()
        self.cache = self.root / 'cache'; self.cache.mkdir()
        self.destination = self.root / 'backup'; self.destination.mkdir()
        # No test may invoke a real package manager, including fetch fallback.
        command_patch = patch.object(update, 'run')
        self.command = command_patch.start()
        self.addCleanup(command_patch.stop)
        resolver_patch = patch.object(update, 'output', return_value='')
        self.resolve_urls = resolver_patch.start()
        self.addCleanup(resolver_patch.stop)
        download_patch = patch.object(update, 'urlopen', side_effect=AssertionError('Unexpected fixture download'))
        self.download = download_patch.start()
        self.addCleanup(download_patch.stop)

    def cached(self, filename, data=b'original APK fixture'):
        return self.write('cache/'+filename, data)

    def test_plain_cached_filename_is_copied_without_fetch(self):
        source = self.cached('libcamera-99.7.1-r0.apk')
        update.preserve_packages(['libcamera=99.7.1-r0'], self.destination, self.cache)
        self.command.assert_not_called()
        backup = self.destination / source.name
        self.assertEqual(backup.read_bytes(), source.read_bytes())
        source.write_bytes(b'cache later changed')
        self.assertEqual(backup.read_bytes(), b'original APK fixture')

    def test_hashed_cache_name_becomes_the_canonical_backup_name(self):
        source = self.cached('pipewire-libs-1.6.8-r4.1234abcd.apk')
        self.cached('pipewire-libs-1.6.8-r40.1234abcd.apk', b'wrong version')
        update.preserve_packages(['pipewire-libs=1.6.8-r4'], self.destination, self.cache)
        self.command.assert_not_called()
        self.assertEqual(sorted(p.name for p in self.destination.iterdir()), ['pipewire-libs-1.6.8-r4.apk'])
        self.assertEqual((self.destination/'pipewire-libs-1.6.8-r4.apk').read_bytes(), source.read_bytes())

    def test_identical_plain_and_hashed_duplicates_are_accepted(self):
        for name in ('dep-2-r0.apk', 'dep-2-r0.aaaaaaaa.apk', 'dep-2-r0.bbbbbbbb.apk'):
            self.cached(name, b'identical bytes')
        update.preserve_packages(['dep=2-r0'], self.destination, self.cache)
        self.command.assert_not_called()
        self.assertEqual((self.destination/'dep-2-r0.apk').read_bytes(), b'identical bytes')
        self.assertEqual(len(list(self.destination.iterdir())), 1)

    def test_conflicting_cache_entries_stop_before_copy_or_fetch(self):
        self.cached('dep-2-r0.apk', b'first contents')
        self.cached('dep-2-r0.aaaaaaaa.apk', b'different contents')
        with self.assertRaisesRegex(ValueError, 'Conflicting cached original APKs'):
            update.preserve_packages(['dep=2-r0'], self.destination, self.cache)
        self.command.assert_not_called()
        self.assertEqual(list(self.destination.iterdir()), [])

    def test_fetch_receives_only_missing_exact_versions(self):
        self.cached('available-1-r0.aabbccdd.apk', b'cached bytes')
        self.cached('missing-1-r0.apk', b'wrong version')
        specs = ['available=1-r0', 'missing=2-r0', 'another=3-r0']
        expected_urls = ['https://repo.example.test/aarch64/missing-2-r0.apk',
                         'https://repo.example.test/aarch64/another-3-r0.apk']
        # The resolver may list dependencies and repeat a URL. Neither causes
        # extra downloads; only the requested exact originals are backed up.
        self.resolve_urls.return_value = '\n'.join([
            *expected_urls, expected_urls[0],
            'https://repo.example.test/aarch64/base-dependency-1-r0.apk',
            'https://repo.example.test/aarch64/available-1-r0.apk'])

        def download(url, timeout):
            self.assertIn(url, expected_urls)
            self.assertEqual(timeout, 30)
            self.assertEqual((self.destination/'available-1-r0.apk').read_bytes(), b'cached bytes')
            return io.BytesIO(('downloaded '+url.rsplit('/', 1)[-1]).encode())

        self.download.side_effect = download
        update.preserve_packages(specs, self.destination, self.cache)
        self.resolve_urls.assert_called_once_with(
            ['apk', 'fetch', '--recursive', '--simulate', '--url', 'missing=2-r0', 'another=3-r0'])
        self.assertEqual([call.args[0] for call in self.download.call_args_list], expected_urls)
        self.command.assert_not_called()
        self.assertEqual(sorted(p.name for p in self.destination.iterdir()),
                         ['another-3-r0.apk', 'available-1-r0.apk', 'missing-2-r0.apk'])
        self.assertEqual((self.destination/'missing-2-r0.apk').read_bytes(), b'downloaded missing-2-r0.apk')
        self.assertEqual((self.destination/'another-3-r0.apk').read_bytes(), b'downloaded another-3-r0.apk')

    def test_ambiguous_wrong_version_or_non_http_urls_are_refused(self):
        fixtures = {
            'ambiguous': 'https://one.example.test/dep-2-r0.apk\nhttps://two.example.test/dep-2-r0.apk',
            'wrong-version': 'https://repo.example.test/dep-3-r0.apk',
            'local-file': 'file:///cache/dep-2-r0.apk',
            'empty': '',
        }
        for name, urls in fixtures.items():
            with self.subTest(case=name):
                self.resolve_urls.return_value = urls
                with self.assertRaisesRegex(ValueError, 'Original APK URL is missing or ambiguous'):
                    update.preserve_packages(['dep=2-r0'], self.destination, self.cache)
                self.download.assert_not_called()
                self.assertEqual(list(self.destination.iterdir()), [])

    def test_symlink_does_not_conflict_with_a_regular_cached_file(self):
        victim = self.write('outside', b'untrusted linked bytes')
        (self.cache/'dep-2-r0.apk').symlink_to('../outside')
        self.cached('dep-2-r0.aaaaaaaa.apk', b'regular cached bytes')
        update.preserve_packages(['dep=2-r0'], self.destination, self.cache)
        self.command.assert_not_called()
        self.assertEqual((self.destination/'dep-2-r0.apk').read_bytes(), b'regular cached bytes')
        self.assertEqual(victim.read_bytes(), b'untrusted linked bytes')

    def test_only_symlink_candidates_are_missing_and_must_be_fetched(self):
        victim = self.write('outside', b'untrusted linked bytes')
        for name in ('dep-2-r0.apk', 'dep-2-r0.aaaaaaaa.apk'):
            (self.cache/name).symlink_to('../outside')
        (self.cache/'dep-2-r0.bbbbbbbb.apk').symlink_to('../absent')
        url = 'https://repo.example.test/dep-2-r0.apk'
        self.resolve_urls.return_value = url
        self.download.side_effect = lambda *args, **kw: io.BytesIO(b'downloaded original')
        update.preserve_packages(['dep=2-r0'], self.destination, self.cache)
        self.resolve_urls.assert_called_once_with(['apk', 'fetch', '--recursive', '--simulate', '--url', 'dep=2-r0'])
        self.download.assert_called_once_with(url, timeout=30)
        self.assertEqual((self.destination/'dep-2-r0.apk').read_bytes(), b'downloaded original')
        self.assertEqual(victim.read_bytes(), b'untrusted linked bytes')


class ConfigRecovery(TemporaryFiles):
    def fixture_record(self, status='APPLIED'):
        self.write('etc/config', b'original', 0o640)
        self.write('etc/apk/world', b'base\n')
        (self.root/'etc/link').symlink_to('config')
        names = ['etc/config', 'etc/link', 'etc/created', 'etc/absent', 'etc/apk/world']
        before = update.snapshot(names)
        self.write('etc/config', b'APK config', 0o644)
        apk = update.snapshot(names)
        self.write('etc/config', b'runtime config', 0o644)
        self.write('etc/created', b'runtime created', 0o644)
        after = update.snapshot(names)
        return {'status': status, 'configs': before, 'config_states': {'apk': apk, 'after': after}}

    def test_applied_restore_rejects_external_changes_before_any_write(self):
        for change in ('edit', 'chmod', 'link', 'created', 'removed', 'world'):
            with self.subTest(change=change), patch.object(update, 'Path', side_effect=self.rooted_path):
                for name in ('etc/link', 'etc/created', 'etc/absent'):
                    p = self.root/name
                    if p.exists() or p.is_symlink(): p.unlink()
                record = self.fixture_record()
                if change == 'edit': self.write('etc/config', b'admin edit')
                if change == 'chmod': (self.root/'etc/config').chmod(0o600)
                if change == 'link':
                    (self.root/'etc/link').unlink(); (self.root/'etc/link').symlink_to('elsewhere')
                if change == 'created': self.write('etc/absent', b'admin added')
                if change == 'removed': (self.root/'etc/created').unlink()
                if change == 'world': self.write('etc/apk/world', b'admin package selection\n')
                actual = update.snapshot(record['configs'])
                with patch.object(update, 'write_configs') as writes:
                    with self.assertRaisesRegex(ValueError, 'Configurations changed outside.*etc/'):
                        update.restore_configs(record)
                    writes.assert_not_called()
                self.assertEqual(update.snapshot(record['configs']), actual)

    def test_interrupted_apply_and_restore_accept_only_known_states(self):
        with patch.object(update, 'Path', side_effect=self.rooted_path):
            record = self.fixture_record('APPLYING')
            for status in ('APPLYING', 'RESTORING'):
                record['status'] = status
                # Mix before, package and final runtime objects as a partial
                # transaction would; all untouched paths must still match.
                mixed = dict(record['config_states']['after'])
                mixed['etc/config'] = record['config_states']['apk']['etc/config']
                mixed['etc/link'] = record['configs']['etc/link']
                update.write_configs(mixed)
                update.restore_configs(record)
                self.assertEqual(update.snapshot(record['configs']), record['configs'])
                self.write('etc/config', b'unknown partial write')
                with self.assertRaisesRegex(ValueError, 'etc/config'):
                    update.restore_configs(record)

    def test_applied_requires_after_even_when_current_equals_before(self):
        with patch.object(update, 'Path', side_effect=self.rooted_path):
            record = self.fixture_record()
            update.write_configs(record['configs'])
            with self.assertRaisesRegex(ValueError, 'etc/config'):
                update.config_guard(record)

    def test_owner_metadata_and_missing_expectations_fail_closed(self):
        with patch.object(update, 'Path', side_effect=self.rooted_path):
            record = self.fixture_record()
            record['config_states']['after']['etc/config']['uid'] += 1
            with self.assertRaisesRegex(ValueError, 'etc/config'): update.config_guard(record)
            del record['config_states']
            with self.assertRaisesRegex(ValueError, 'reviewed recovery'): update.config_guard(record)

    def test_atomic_config_failure_preserves_original_object(self):
        with patch.object(update, 'Path', side_effect=self.rooted_path):
            record = self.fixture_record()
            actual = update.snapshot(record['configs'])
            with patch.object(update.os, 'replace', side_effect=OSError('interrupted replacement')):
                with self.assertRaises(OSError): update.write_configs(record['configs'])
            self.assertEqual(update.snapshot(record['configs']), actual)
            self.assertEqual(list(self.root.rglob('.duet-config-*')), [])

    def test_restores_file_bytes_mode_links_and_original_absence(self):
        original = b'original config\x00\xff\n'
        regular = self.write('etc/config', original, 0o640)
        link = self.root / 'etc/unit-link'
        link.symlink_to('config')
        dangling = self.root / 'etc/dangling-link'
        dangling.symlink_to('missing-target')
        created = self.root / 'etc/new-config'
        names = ['etc/config', 'etc/unit-link', 'etc/dangling-link', 'etc/new-config']
        with patch.object(update, 'Path', side_effect=self.rooted_path):
            saved = update.snapshot(names)
            regular.write_bytes(b'updated'); regular.chmod(0o755)
            link.unlink(); link.write_bytes(b'now regular')
            dangling.unlink(); dangling.symlink_to('different-target')
            created.write_bytes(b'added by update')
            update.restore_configs({'status': 'APPLIED', 'configs': saved, 'config_states': {'apk': update.snapshot(saved), 'after': update.snapshot(saved)}})
            self.assertEqual(update.snapshot(names), saved)
        self.assertEqual(regular.read_bytes(), original)
        self.assertEqual(stat.S_IMODE(regular.stat().st_mode), 0o640)
        self.assertEqual(os.readlink(link), 'config')
        self.assertEqual(os.readlink(dangling), 'missing-target')
        self.assertFalse(created.exists())
        self.assertFalse(created.is_symlink())

    def test_restore_unlinks_replacement_symlink_without_writing_its_target(self):
        config = self.write('etc/config', b'original')
        victim = self.write('unrelated', b'keep unchanged')
        with patch.object(update, 'Path', side_effect=self.rooted_path):
            saved = update.snapshot(['etc/config', 'etc/absent'])
            config.unlink(); config.symlink_to('../unrelated')
            absent = self.root / 'etc/absent'; absent.symlink_to('../unrelated')
            update.restore_configs({'status': 'APPLIED', 'configs': saved, 'config_states': {'apk': update.snapshot(saved), 'after': update.snapshot(saved)}})
        self.assertEqual(config.read_bytes(), b'original')
        self.assertFalse(config.is_symlink())
        self.assertFalse(absent.is_symlink())
        self.assertEqual(victim.read_bytes(), b'keep unchanged')

    def test_symlink_parent_is_rejected_before_any_restore_write(self):
        config = self.write('etc/config', b'original')
        nested = self.write('etc/nested/config', b'original nested')
        victim = self.write('outside/config', b'keep unchanged')
        with patch.object(update, 'Path', side_effect=self.rooted_path):
            saved = update.snapshot(['etc/config', 'etc/nested/config'])
            config.write_bytes(b'update still present')
            nested.unlink(); nested.parent.rmdir(); nested.parent.symlink_to('../outside')
            with self.assertRaisesRegex(ValueError, 'Symlink parent'):
                update.restore_configs({'status': 'APPLIED', 'configs': saved, 'config_states': {'apk': update.snapshot(saved), 'after': update.snapshot(saved)}})
        self.assertEqual(config.read_bytes(), b'update still present')
        self.assertEqual(victim.read_bytes(), b'keep unchanged')

    def test_directory_and_parent_traversal_are_refused(self):
        (self.root / 'etc/directory').mkdir(parents=True)
        with patch.object(update, 'Path', side_effect=self.rooted_path):
            with self.assertRaisesRegex(ValueError, 'regular config'):
                update.snapshot(['etc/directory'])
            with self.assertRaisesRegex(ValueError, 'Invalid backup path'):
                update.snapshot(['../outside'])


class RestoreAdmission(TemporaryFiles):
    fixture_record = ConfigRecovery.fixture_record
    def test_main_restores_and_persists_package_config_expectations_before_writes(self):
        with patch.object(update, 'Path', side_effect=self.rooted_path): record = self.fixture_record()
        record.update(identity={'/': 'fixture-root', '/boot': 'fixture-boot'},
            before={'base': '1'}, after={'base': '2'}, old_apks={}, repository_indexes=[],
            world=base64.b64encode(b'base\n').decode(), old_specs=['base=1'])
        directory = self.root/'state/backup'; directory.mkdir(parents=True)
        (directory.parent/'latest').symlink_to(directory.name); update.save_state(record, directory)
        restored_apk = dict(record['config_states']['after'])
        # The old APK proposes a config sidecar before saved objects are restored.
        restored_apk['etc/config'] = record['configs']['etc/config']
        original_stat = Path.stat
        def fixture_stat(path, **kwargs):
            st = original_stat(path, **kwargs)
            if path == directory.parent:
                fields = list(st); fields[4] = 0; return os.stat_result(fields)
            return st
        def install_old(*args, **kwargs):
            persisted = json.loads((directory/'state.json').read_text())
            self.assertEqual(persisted['status'], 'RESTORING')
            self.assertEqual(persisted['config_states']['restore_apk'], restored_apk)
            update.write_configs(restored_apk)
            return 'restored APK fixture'
        with patch.object(sys, 'argv', [str(SCRIPT), 'restore', '--user', 'desktop']), \
                patch.object(Path, 'stat', autospec=True, side_effect=fixture_stat), \
                patch.object(update, 'STATE', directory.parent), \
                patch.object(update, 'Path', side_effect=self.rooted_path), \
                patch.object(update, 'checker_zstd', return_value=contextlib.nullcontext(None)), \
                patch.object(update, 'usb_gate', return_value=record['identity']), \
                patch.object(update, 'packages', side_effect=[record['after'], record['before']]), \
                patch.object(update, 'sandbox', return_value=(record['before'], 'plan', restored_apk)), \
                patch.object(update, 'stop_session'), patch.object(update, 'run'), \
                patch.object(update, 'output', return_value='fixture-kernel'), \
                patch.object(update, 'apk_add', side_effect=install_old):
            update.main()
            self.assertEqual(update.snapshot(record['configs']), record['configs'])
        self.assertEqual(json.loads((directory/'state.json').read_text())['status'], 'RESTORED')

    def test_main_refuses_config_changes_before_package_world_or_session_changes(self):
        with patch.object(update, 'Path', side_effect=self.rooted_path):
            record = self.fixture_record()
        record.update(identity={'/': 'fixture-root', '/boot': 'fixture-boot'},
            before={'base': '1'}, after={'base': '2'}, old_apks={}, repository_indexes=[],
            world=base64.b64encode(b'base\n').decode(), old_specs=['base=1'])
        directory = self.root/'state/backup'; directory.mkdir(parents=True)
        (directory.parent/'latest').symlink_to(directory.name)
        update.save_state(record, directory)
        self.write('etc/config', b'admin change')
        state_bytes = (directory/'state.json').read_bytes()
        world_bytes = (self.root/'etc/apk/world').read_bytes()
        original_stat = Path.stat
        def fixture_stat(path, **kwargs):
            st = original_stat(path, **kwargs)
            if path == directory.parent:
                fields = list(st); fields[4] = 0; return os.stat_result(fields)
            return st
        with patch.object(sys, 'argv', [str(SCRIPT), 'restore', '--user', 'desktop']), \
                patch.object(Path, 'stat', autospec=True, side_effect=fixture_stat), \
                patch.object(update, 'STATE', directory.parent), \
                patch.object(update, 'Path', side_effect=self.rooted_path), \
                patch.object(update, 'checker_zstd', return_value=contextlib.nullcontext(None)), \
                patch.object(update, 'usb_gate', return_value=record['identity']), \
                patch.object(update, 'packages', return_value=record['after']), \
                patch.object(update, 'apk_add') as apk, \
                patch.object(update, 'sandbox') as sandbox, \
                patch.object(update, 'stop_session') as stop, \
                patch.object(update, 'write_configs') as write:
            with self.assertRaisesRegex(ValueError, 'etc/config'): update.main()
            for command in (apk, sandbox, stop, write): command.assert_not_called()
        self.assertEqual((directory/'state.json').read_bytes(), state_bytes)
        self.assertEqual((self.root/'etc/apk/world').read_bytes(), world_bytes)
        self.assertEqual((self.root/'etc/config').read_bytes(), b'admin change')


class SandboxConfigs(TemporaryFiles):
    def test_package_prediction_uses_original_config_and_records_apk_new(self):
        self.write('lib/apk/db/installed', b'P:base\nV:1\n\n')
        self.write('etc/apk/world', b'base\n')
        self.write('etc/config', b'admin original', 0o640)
        (self.root/'etc/link').symlink_to('config')
        names = ['etc/config', 'etc/config.apk-new', 'etc/link', 'etc/apk/world']
        def fake_apk(root, *args, **kwargs):
            root = Path(root)
            self.assertEqual((root/'etc/config').read_bytes(), b'admin original')
            self.assertEqual(stat.S_IMODE((root/'etc/config').stat().st_mode), 0o640)
            self.assertEqual(os.readlink(root/'etc/link'), 'config')
            (root/'etc/config.apk-new').write_bytes(b'package proposal')
            (root/'lib/apk/db/installed').write_text('P:base\nV:2\n\n')
            return 'APK fixture installed'
        with patch.object(update, 'apk_add', side_effect=fake_apk):
            planned, log, configs = update.sandbox([], self.root/'keys', source_root=self.root, config_paths=names)
        self.assertEqual(planned, {'base': '2'})
        self.assertEqual(base64.b64decode(configs['etc/config.apk-new']['data']), b'package proposal')
        self.assertEqual(base64.b64decode(configs['etc/config']['data']), b'admin original')
        self.assertFalse((self.root/'etc/config.apk-new').exists())


class ApplyAdmission(TemporaryFiles):
    def test_apply_records_expected_configs_before_install_and_checks_readback(self):
        self.write('etc/apk/world', b'camera\n')
        self.write('etc/apk/keys/original.pub', b'OS key')
        self.write('etc/config', b'original runtime', 0o640)
        key = self.write('incoming/camera.pub', b'fixture public key')
        apk = self.write('incoming/camera-2.apk', b'fixture APK')
        state = self.root/'state'
        before, after = {'camera': '1'}, {'camera': '2'}
        original_stat, original_copytree = Path.stat, update.shutil.copytree
        predictions = {}
        def rooted(value):
            p = Path(value)
            if p.parent == Path(tempfile.gettempdir()) and p.name.startswith('duet-trusted-'):
                return p
            return self.rooted_path(value)
        def fixture_stat(path, **kwargs):
            st = original_stat(path, **kwargs)
            if path.is_relative_to(self.root):
                fields = list(st); fields[4] = fields[5] = 0; return os.stat_result(fields)
            return st
        def copytree(source, destination, *args, **kwargs):
            source = self.root/'etc/apk/keys' if source == '/etc/apk/keys' else source
            return original_copytree(source, destination, *args, **kwargs)
        def predict(apks, keys, **kwargs):
            if 'config_paths' not in kwargs: return before, 'restore plan'
            configs = update.snapshot(kwargs['config_paths'])
            configs['etc/apk/world'] = dict(configs['etc/apk/world'], data=base64.b64encode(b'camera=2\n').decode())
            configs['etc/modprobe.d/duet-camera.conf'] = {'data':base64.b64encode(b'package config').decode(), 'mode':0o644,'uid':0,'gid':0}
            predictions.update(configs)
            return after, 'install plan', configs
        def preserve(specs, destination): (destination/'camera-1.apk').write_bytes(b'old APK')
        def install(*args, **kwargs):
            record = json.loads((state/'latest/state.json').read_text())
            self.assertEqual(record['status'], 'APPLYING')
            self.assertEqual(record['config_states']['apk'], predictions)
            self.assertEqual(base64.b64decode(record['config_states']['after']['etc/config']['data']), b'installed runtime')
            update.write_configs(predictions)
            return 'installed APK fixture'
        with patch.object(sys, 'argv', [str(SCRIPT), 'apply', '--user', 'desktop']), \
                patch.object(Path, 'stat', autospec=True, side_effect=fixture_stat), \
                patch.object(update, 'STATE', state), \
                patch.object(update, 'Path', side_effect=rooted), \
                patch.object(update.os, 'fchown'), \
                patch.object(update.shutil, 'copytree', side_effect=copytree), \
                patch.object(update, 'checker_zstd', return_value=contextlib.nullcontext(None)), \
                patch.object(update, 'usb_gate', return_value={'/':'fixture-root','/boot':'fixture-boot'}), \
                patch.object(update, 'PINS', {'camera':'2'}), \
                patch.object(update, 'packages', side_effect=[before, before, after]), \
                patch.object(update, 'selected_apks', return_value=([apk], key)), \
                patch.object(update, 'selected_zstd', return_value=[]), \
                patch.object(update, 'sandbox', side_effect=predict), \
                patch.object(update, 'runtime_files', return_value={'etc/config':b'installed runtime'}), \
                patch.object(update, 'preserve_packages', side_effect=preserve), \
                patch.object(update, 'preserve_repositories', return_value=[]), \
                patch.object(update, 'stop_session'), patch.object(update, 'run'), \
                patch.object(update, 'output', return_value='fixture-kernel'), \
                patch.object(update, 'apk_add', side_effect=install):
            update.main()
            record = json.loads((state/'latest/state.json').read_text())
            self.assertEqual(record['status'], 'APPLIED')
            self.assertEqual(update.snapshot(record['configs']), record['config_states']['after'])
            self.assertEqual(base64.b64decode(record['configs']['etc/config']['data']), b'original runtime')


class FinishRecovery(TemporaryFiles):
    def test_each_failed_post_transaction_step_can_be_retried(self):
        for action, pending, completed in [('apply', 'APPLYING', 'APPLIED'),
                                           ('restore', 'RESTORING', 'RESTORED')]:
            for fail_at in range(3):
                with self.subTest(action=action, fail_at=fail_at):
                    directory = self.root / (action+str(fail_at)); directory.mkdir()
                    record = {'status': pending, 'retained': 'backup metadata'}
                    update.save_state(record, directory)
                    calls = []

                    def command(args):
                        # Both memory and durable state must remain resumable
                        # until every external post-transaction step succeeds.
                        self.assertEqual(record['status'], pending)
                        self.assertEqual(json.loads((directory/'state.json').read_text())['status'], pending)
                        calls.append(args)
                        if len(calls)-1 == fail_at:
                            raise subprocess.CalledProcessError(1, args)

                    with patch.object(update, 'output', return_value='6.18.28-mt81'), \
                            patch.object(update, 'run', side_effect=command):
                        with self.assertRaises(subprocess.CalledProcessError):
                            update.finish_update(action, record, directory)
                    self.assertEqual(len(calls), fail_at+1)
                    resumed = json.loads((directory/'state.json').read_text())
                    self.assertEqual(resumed, {'status': pending, 'retained': 'backup metadata'})
                    with patch.object(update, 'output', return_value='6.18.28-mt81'), \
                            patch.object(update, 'run'):
                        update.finish_update(action, resumed, directory)
                    self.assertEqual(json.loads((directory/'state.json').read_text()),
                                     {'status': completed, 'retained': 'backup metadata'})
                    self.assertFalse((directory/'state.tmp').exists())


if __name__ == '__main__':
    unittest.main()
