#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise update storage gates on fixtures; never touch real devices or mounts."""
import contextlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1]/'scripts/apply-update.py'
spec = importlib.util.spec_from_file_location('update_storage_under_test', SCRIPT)
update = importlib.util.module_from_spec(spec)
spec.loader.exec_module(update)


class StorageGates(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='duet-update-storage-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name).resolve()
        self.mounts = {'/': '/dev/mmcblk0p3', '/boot': '/dev/mmcblk0p2'}
        self.options = {'/': 'rw,relatime', '/boot': 'ro,nosuid,nodev,noexec,relatime'}
        self.uuids = {'mmcblk0p3': 'root-uuid', 'mmcblk0p2': 'boot-uuid',
                      'sda3': 'usb-root', 'sda2': 'usb-boot'}
        self.calls = []
        self.compatible = True
        self.apk_version = 'apk-tools 3.0.2'
        self.disk = self.make_disk('mmcblk0')
        self.partition(self.disk, 'mmcblk0p2', '2')
        self.partition(self.disk, 'mmcblk0p3', '3')
        for suffix in ('boot0', 'boot1'):
            node = self.write('sys/devices/controller/block/mmcblk0'+suffix+'/ro', '1').parent
            self.link('sys/class/block/mmcblk0'+suffix, node)
            self.link('sys/block/mmcblk0'+suffix, node)
        usb = self.root/'sys/devices/usb1/1-1/block/sda'
        self.partition(usb, 'sda2', '2')
        self.partition(usb, 'sda3', '3')

    def write(self, path, data):
        target = self.root/path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(data)
        return target

    def link(self, path, target):
        link = self.root/path
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(target)

    def make_disk(self, name):
        disk = self.root/'sys/devices/controller/block'/name
        self.write(str(disk.relative_to(self.root))+'/device/type', 'MMC')
        self.write(str(disk.relative_to(self.root))+'/device/cid', '00112233445566778899aabbccddeeff')
        self.write(str(disk.relative_to(self.root))+'/ro', '0')
        self.link('sys/block/'+name, disk)
        return disk

    def partition(self, disk, name, number):
        node = disk/name
        self.write(str(node.relative_to(self.root))+'/partition', number)
        self.write(str(node.relative_to(self.root))+'/ro', '0')
        self.link('sys/class/block/'+name, node)
        self.write('dev/'+name, 'fixture block path')

    def rooted_path(self, value):
        p = Path(value)
        return self.root/str(p).lstrip('/') if p.is_absolute() else p

    def command(self, args):
        args = list(map(str, args)); self.calls.append(args)
        if args[0] == 'findmnt':
            return (self.options if args[3] == 'OPTIONS' else self.mounts)[args[-1]]
        if args[0] == 'blkid':
            return self.uuids[Path(args[-1]).name]
        if args[0] == 'python3':
            return json.dumps({'compatible': self.compatible})
        if args == ['apk', '--version']:
            return self.apk_version
        raise AssertionError('Unexpected command: '+repr(args))

    def invoke(self, internal=True):
        with patch.object(update, 'Path', side_effect=self.rooted_path), \
                patch.object(update, 'output', side_effect=self.command), \
                patch.object(update.os, 'geteuid', return_value=0):
            return update.internal_gate() if internal else update.usb_gate()

    def rejected(self, pattern=None, internal=True):
        with self.assertRaisesRegex(ValueError, pattern or '.'):
            self.invoke(internal)

    def test_matched_internal_mounts_record_cid_and_both_uuids(self):
        self.assertEqual(self.invoke(), {'/': 'root-uuid', '/boot': 'boot-uuid',
                                        'emmc_cid': '00112233445566778899aabbccddeeff'})
        self.assertTrue(any(c[0] == 'python3' for c in self.calls))
        self.assertTrue(all(c[0] in ('findmnt', 'blkid', 'python3', 'apk') for c in self.calls))

    def test_internal_requires_read_only_boot_and_writable_root(self):
        for mount, options in (('/boot', 'rw'), ('/', 'ro')):
            with self.subTest(mount=mount):
                original = self.options[mount]; self.options[mount] = options
                self.rejected('read-only'); self.options[mount] = original

    def test_hardware_boot_regions_must_remain_protected(self):
        for suffix in ('boot0', 'boot1'):
            with self.subTest(region=suffix):
                path = self.root/'sys/class/block'/('mmcblk0'+suffix)/'ro'
                path.write_text('0'); self.rejected('boot protection'); path.write_text('1')

    def test_internal_rejects_usb_overlay_and_sd(self):
        self.write('dev/mapper/root', 'fixture mapper path')
        for source in ('/dev/sda3', 'overlay', 'tmpfs', '/dev/mapper/root'):
            with self.subTest(source=source):
                self.mounts['/'] = source; self.rejected()
        self.mounts['/'] = '/dev/mmcblk0p3'
        (self.disk/'device/type').write_text('SD'); self.rejected('SD card')

    def test_internal_rejects_mixed_emmc_devices(self):
        disk = self.make_disk('mmcblk1')
        self.partition(disk, 'mmcblk1p2', '2')
        self.uuids['mmcblk1p2'] = 'other-boot'
        self.mounts['/boot'] = '/dev/mmcblk1p2'
        self.rejected('same eMMC')

    def test_internal_rejects_shared_partition_or_duplicate_uuid(self):
        self.mounts['/boot'] = '/dev/mmcblk0p3'; self.rejected('separate partitions')
        self.mounts['/boot'] = '/dev/mmcblk0p2'
        self.uuids['mmcblk0p2'] = 'root-uuid'; self.rejected('UUIDs must differ')

    def test_empty_or_multiline_uuid_and_bad_cid_are_refused(self):
        for uuid in ('', 'first\nsecond'):
            with self.subTest(uuid=uuid):
                self.uuids['mmcblk0p3'] = uuid; self.rejected('UUID')
        self.uuids['mmcblk0p3'] = 'root-uuid'
        (self.disk/'device/cid').write_text('unknown'); self.rejected('CID')

    def test_kernel_dt_firmware_or_apk_mismatch_still_rejected(self):
        self.compatible = False; self.rejected('kernel, camera DT and firmware')
        self.compatible = True; self.apk_version = 'apk-tools 2.14'; self.rejected('apk-tools 3')

    def test_default_usb_path_rejects_internal_os(self):
        self.rejected('USB partitions', internal=False)

    def test_usb_identity_stays_compatible_with_saved_backups(self):
        self.mounts = {'/': '/dev/sda3', '/boot': '/dev/sda2'}
        for ro in (self.disk/'ro', *(self.disk.glob('mmcblk0p*/ro'))):
            ro.write_text('1')
        self.assertEqual(self.invoke(False), {'/': 'usb-root', '/boot': 'usb-boot'})
        (self.disk/'ro').write_text('0'); self.rejected('eMMC protection', internal=False)


class BackupModes(unittest.TestCase):
    def test_legacy_usb_backups_still_match_only_usb(self):
        identity = {'/': 'root', '/boot': 'boot'}
        legacy = {'identity': identity}
        self.assertTrue(update.backup_matches(legacy, identity, 'external-usb'))
        self.assertFalse(update.backup_matches(legacy, identity, 'internal-emmc'))

    def test_mode_cid_or_uuid_change_rejects_restore(self):
        identity = {'/': 'root', '/boot': 'boot', 'emmc_cid': 'a'*32}
        record = {'mode': 'internal-emmc', 'identity': identity}
        self.assertTrue(update.backup_matches(record, identity, 'internal-emmc'))
        self.assertFalse(update.backup_matches(record, identity, 'external-usb'))
        for field in identity:
            with self.subTest(field=field):
                self.assertFalse(update.backup_matches(record, dict(identity, **{field: 'different'}), 'internal-emmc'))

    def test_main_selects_internal_gate_only_with_explicit_argument(self):
        for arguments, expected in (([], 'usb'), (['--internal-emmc'], 'internal')):
            with self.subTest(arguments=arguments), \
                    patch.object(sys, 'argv', [str(SCRIPT), 'check', *arguments]), \
                    patch.object(update, 'checker_zstd', return_value=contextlib.nullcontext(None)), \
                    patch.object(update, 'usb_gate', side_effect=ValueError('usb')) as usb, \
                    patch.object(update, 'internal_gate', side_effect=ValueError('internal')) as internal:
                with self.assertRaisesRegex(ValueError, '^'+expected+'$'):
                    update.main()
                self.assertEqual(usb.call_count, int(expected == 'usb'))
                self.assertEqual(internal.call_count, int(expected == 'internal'))

    def test_media_autostart_is_limited_to_selected_user(self):
        from types import SimpleNamespace
        with patch.object(update.pwd,'getpwnam',return_value=SimpleNamespace(pw_uid=10000)):
            files=update.runtime_files(True,'desktop')
        service=files['usr/lib/systemd/user/duet-camera-user-media.service']
        self.assertIn(b'ConditionUser=desktop\n',service)
        self.assertIn(b'ExecStart=/usr/bin/systemctl --user start pipewire.service wireplumber.service',service)
        self.assertNotIn('etc/systemd/user/default.target.wants/pipewire.service',files)
        self.assertEqual(files['etc/systemd/user/default.target.wants/duet-camera-user-media.service'],('link','/usr/lib/systemd/user/duet-camera-user-media.service'))

    def test_runtime_modes_select_explicit_service_arguments(self):
        external = update.runtime_files()
        internal = update.runtime_files(True)
        init = 'usr/lib/systemd/system/duet-camera-scp-init.service'
        activate = 'usr/lib/systemd/system/duet-camera-activate.service'
        config = 'etc/modprobe.d/duet-camera-scp-isolation.conf'
        self.assertIn(b'ExecStart=/usr/libexec/duet-camera-external-scp-init\n', external[init])
        self.assertIn(b'ExecStart=/usr/libexec/duet-camera-external-scp-init --internal-emmc\n', internal[init])
        self.assertIn(b'ExecStart=/usr/bin/duet-camera-activate --external-usb\n', external[activate])
        self.assertIn(b'ExecStart=/usr/bin/duet-camera-activate --internal-emmc\n', internal[activate])
        self.assertNotEqual(external[config], internal[config])
        self.assertEqual(set(external), set(internal))
        self.assertTrue(all(not name.startswith('boot/') for name in internal))
        for name in set(external) - {init, activate, config}:
            self.assertEqual(external[name], internal[name], name)


if __name__ == '__main__':
    unittest.main()
