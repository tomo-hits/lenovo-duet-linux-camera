#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host-only boot admission, relocation and mount restoration failure tests."""
import contextlib, importlib.util, json, pathlib, struct, sys, tempfile, types, unittest
from unittest.mock import patch
HERE=pathlib.Path(__file__).resolve().parents[1]/'scripts'
spec=importlib.util.spec_from_file_location('boot_update',HERE/'boot-update.py');boot=importlib.util.module_from_spec(spec);spec.loader.exec_module(boot)

class BootSafety(unittest.TestCase):
    def test_fstab_changes_only_boot_and_preserves_root(self):
        text='# original\nUUID=a / ext4 defaults 0 0\nUUID=b /boot ext2 rw,nodev,nosuid,noexec 0 0\n'
        result=boot.protect_boot_mount(text)
        self.assertTrue(result.startswith('# original\nUUID=a / ext4 defaults 0 0\n'))
        self.assertEqual(result.splitlines()[-1].split(),['UUID=b','/boot','ext2','ro,nodev,nosuid,noexec','0','0'])
        self.assertEqual(boot.protect_boot_mount(result),result)

    def test_absent_duplicate_or_ambiguous_boot_is_refused(self):
        for text in ('UUID=a / ext4 defaults 0 0\n','UUID=b /boot ext2 defaults 0 0\n'*2,'UUID=b /boot ext2 defaults 0 0 # comment\n'):
            with self.assertRaises(RuntimeError):boot.protect_boot_mount(text)

    def test_failed_write_restores_original_mount_protection(self):
        p={'original_boot_mount_options':'rw,nodev'}
        calls=[]
        def fail():raise RuntimeError('write failed')
        with patch.object(boot.tx,'mounts',return_value={'/boot':{'options':'ro,nodev'}}),patch.object(boot.tx,'run',side_effect=lambda args:calls.append(args)):
            with self.assertRaises(RuntimeError):boot.mounted(p,fail)
        self.assertEqual(calls,[['mount','-o','remount,rw','/boot'],['mount','-o','remount,ro','/boot']])

    def test_original_rw_mount_is_restored_after_successful_rollback(self):
        calls=[]
        def restore_action():pass
        with patch.object(boot.tx,'mounts',return_value={'/boot':{'options':'ro,nodev'}}),patch.object(boot.tx,'run',side_effect=lambda args:calls.append(args)):
            boot.mounted({'original_boot_mount_options':'rw,nodev'},restore_action)
        self.assertEqual(calls[-1],['mount','-o','remount,rw','/boot'])

    def test_fstab_permissions_are_preserved_when_staging(self):
        plan={'boot_files':{},'preboot_configs':{'/etc/fstab':'fixture'},'preboot_config_metadata':{'/etc/fstab':{'mode':0o600,'uid':0,'gid':0}}}
        self.assertEqual(boot.tx.expected_after(plan,'/etc/fstab'),{'kind':'file','mode':0o600,'uid':0,'gid':0,'sha256':boot.tx.sha(b'fixture')})

    def test_unsupported_or_truncated_vboot_layout_is_refused(self):
        for blob in (b'',b'CHROMEOS',b'CHROMEOS'+bytes(65528)):
            with self.assertRaises((ValueError,struct.error)):boot.prep.layout(blob)

    def test_user_wrapper_holds_shared_transaction_lock_during_mutation(self):
        events=[]
        @contextlib.contextmanager
        def lock():
            events.append('lock')
            try:yield
            finally:events.append('unlock')
        with patch.object(sys,'argv',['boot-update.py','restore']),patch.object(boot.os,'geteuid',return_value=0), \
                patch.object(boot,'locked_work',return_value=123),patch.object(boot.os,'close'), \
                patch.object(boot.tx,'transaction_lock',side_effect=lock), \
                patch.object(boot,'restore',side_effect=lambda:events.append('restore')):
            boot.main()
        self.assertEqual(events,['lock','restore','unlock'])

class PreparedRecovery(unittest.TestCase):
    def setUp(self):
        temp=tempfile.TemporaryDirectory(prefix='duet-prepared-test-');self.addCleanup(temp.cleanup)
        self.root=pathlib.Path(temp.name).resolve();self.state=self.root/'state';self.state.mkdir()
        target=self.root/'boot-file';target.write_bytes(b'original boot file')
        self.target=str(target);self.absent=str(self.root/'absent-config')
        tx=boot.tx
        self.stack=contextlib.ExitStack();self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.object(tx,'parents_safe'))
        meta,data=tx.snapshot(target)
        self.plan={'raw_size_bytes':len(b'original raw'),'raw_before_sha256':tx.sha(b'original raw'),
            'boot_files':{self.target:{'before_'+k:meta[k] for k in ('mode','uid','gid','bytes','sha256')}},
            'preboot_configs':{self.absent:'new config'}}
        meta['backup_file']='file-0.bin';(self.state/'file-0.bin').write_bytes(data)
        (self.state/'original-p1.bin').write_bytes(b'original raw')
        self.record={'status':'PREPARED','plan_sha256':'fixture-plan','snapshots':{self.target:meta,self.absent:{'kind':'absent'}},'journal':[]}
        (self.state/'state.json').write_text(json.dumps(self.record))
        self.stack.enter_context(patch.object(tx,'PLAN_SHA256','fixture-plan'))
        self.stack.enter_context(patch.object(tx,'state_dir',return_value=self.state))
        # Ownership/device fixtures only; saved digests and live file snapshots
        # use production checks against temporary files.
        read=tx.read_file
        self.stack.enter_context(patch.object(tx,'read_file',side_effect=lambda path,private=False:read(path)))
        self.identity=self.stack.enter_context(patch.object(tx,'identity'))
        self.packages=self.stack.enter_context(patch.object(tx,'package_guard'))
        self.raw=self.stack.enter_context(patch.object(tx,'raw_bytes',return_value=b'original raw'))
        self.stack.enter_context(patch.object(tx,'atomic_bytes',side_effect=lambda path,data,*args:pathlib.Path(path).write_bytes(data)))
        self.mount=self.stack.enter_context(patch.object(tx,'run',side_effect=AssertionError('Unexpected mount/device command')))
        self.write=self.stack.enter_context(patch.object(tx,'write_raw',side_effect=AssertionError('Unexpected raw write')))
        self.restore_file=self.stack.enter_context(patch.object(tx,'restore_snapshot',side_effect=AssertionError('Unexpected target write')))
        self.args=types.SimpleNamespace(state=str(self.state),execute=True)

    def wrapper(self):
        (self.root/'latest.json').write_text('{}')
        self.stack.enter_context(patch.object(boot,'WORK',self.root))
        self.stack.enter_context(patch.object(boot,'latest_work',return_value=(self.root,{'state':str(self.state)})))
        self.stack.enter_context(patch.object(boot.tx,'load_plan',return_value=self.plan))

    def test_restore_cancels_before_mount_and_keeps_all_backups(self):
        self.wrapper();boot.restore()
        self.assertEqual(boot.tx.load_state(self.state)['status'],'CANCELLED_PREPARED')
        self.identity.assert_called_once_with(self.plan)
        self.packages.assert_called_once_with(self.plan)
        self.assertEqual((self.state/'original-p1.bin').read_bytes(),b'original raw')
        self.assertEqual(pathlib.Path(self.target).read_bytes(),b'original boot file')
        self.mount.assert_not_called();self.write.assert_not_called();self.restore_file.assert_not_called()
        boot.restore() # already cancelled is idempotent

    def test_remount_failure_leaves_prepared_and_retry_reaches_new_preparation(self):
        self.wrapper()
        with patch.object(boot.tx,'mounts',return_value={'/boot':{'options':'ro'}}), \
                patch.object(boot.tx,'run',side_effect=RuntimeError('remount failed')), \
                patch.object(boot.tx,'apply') as apply:
            with self.assertRaisesRegex(RuntimeError,'remount failed'):
                boot.mounted(self.plan,lambda:boot.tx.apply(self.args,self.plan))
            apply.assert_not_called()
        self.assertEqual(boot.tx.load_state(self.state)['status'],'PREPARED')
        with patch.object(boot,'current_plan',side_effect=RuntimeError('new preparation reached')):
            with self.assertRaisesRegex(RuntimeError,'new preparation reached'):boot.prepare(self.root/'packages')
        self.assertEqual(boot.tx.load_state(self.state)['status'],'CANCELLED_PREPARED')

    def test_interruption_after_latest_save_can_be_cancelled(self):
        self.wrapper()
        # No apply or remount has run: persisted latest + PREPARED is sufficient.
        boot.restore()
        self.assertEqual(boot.tx.load_state(self.state)['journal'][0]['event'],'prepared_cancelled_unchanged')

    def test_changed_target_raw_identity_package_or_backup_blocks_cancellation(self):
        self.wrapper()
        for failure in ('file','absent','raw','backup','identity','packages'):
            with self.subTest(failure=failure), contextlib.ExitStack() as patches:
                pathlib.Path(self.target).write_bytes(b'original boot file')
                pathlib.Path(self.absent).unlink(missing_ok=True)
                (self.state/'file-0.bin').write_bytes(b'original boot file')
                if failure=='file':pathlib.Path(self.target).write_bytes(b'admin edit')
                if failure=='absent':pathlib.Path(self.absent).write_bytes(b'admin added')
                if failure=='backup':(self.state/'file-0.bin').write_bytes(b'corrupt')
                if failure=='raw':patches.enter_context(patch.object(boot.tx,'raw_bytes',return_value=b'changed raw'))
                if failure=='identity':patches.enter_context(patch.object(boot.tx,'identity',side_effect=RuntimeError('CID differs')))
                if failure=='packages':patches.enter_context(patch.object(boot.tx,'package_guard',side_effect=RuntimeError('Packages differ')))
                with self.assertRaises(RuntimeError):boot.restore()
                self.assertEqual(boot.tx.load_state(self.state)['status'],'PREPARED')
                self.mount.assert_not_called();self.write.assert_not_called()

    def test_applying_and_written_journal_are_never_cancelled(self):
        for status,journal in (('APPLYING',[]),('APPLIED',[]),('PREPARED',[{'event':'apply_started'}])):
            with self.subTest(status=status,journal=journal):
                record=dict(self.record,status=status,journal=journal)
                (self.state/'state.json').write_text(json.dumps(record))
                with self.assertRaisesRegex(RuntimeError,'untouched PREPARED'):boot.tx.cancel_prepared(self.args,self.plan)
                self.identity.assert_not_called();self.write.assert_not_called()

if __name__=='__main__':unittest.main()
