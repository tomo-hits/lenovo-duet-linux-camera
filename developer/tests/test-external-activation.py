#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run activation guards against temporary USB/MMC/sysfs fixtures.

Production main and guard code run unchanged. Absolute filesystem paths are
mapped into a temporary root, and every external command is intercepted.
No real storage, module, camera, network or service is touched.
"""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / 'packaging/postmarketos/duet-camera-activate.py'
spec = importlib.util.spec_from_file_location('external_activation_under_test', SCRIPT)
activate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(activate)


class ExternalActivation(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='duet-activate-test-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.calls = []
        self.mounts = {'/': '/dev/sda3', '/boot': '/dev/sda2'}
        self.selected = {}
        self.compatible = True
        self.partition('sda', 'sda2', usb=True)
        self.partition('sda', 'sda3', usb=True)
        self.write('sys/block/mmcblk0/device/type', 'MMC')
        for name in ('mmcblk0', 'mmcblk0/mmcblk0p1', 'mmcblk0/mmcblk0p3',
                     'mmcblk0boot0', 'mmcblk0boot1'):
            self.write('sys/block/'+name+'/ro', '1')
        self.state = self.write('sys/class/remoteproc/remoteproc0/state', 'offline')
        target=self.root/'sys/bus/platform/drivers/mtk-cam-p1-raw';target.mkdir(parents=True)
        link=self.root/'sys/bus/platform/devices/1a006000.camera/driver';link.parent.mkdir(parents=True);link.symlink_to(target)
        self.write('sys/class/video4linux/video0/name','MT8183 P1 RAW')
        for name in activate.MODULES:
            path = self.write('lib/modules/'+activate.KERNEL+'/extra/duet-camera/'+name+'.ko', 'fixture module')
            self.selected[name] = str(path)

    def write(self, name, value):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value)
        return path

    def partition(self, disk, name, usb):
        bus = 'usb1/1-1' if usb else 'internal-controller'
        node = self.write('sys/devices/'+bus+'/block/'+disk+'/'+name+'/partition', '2').parent
        link = self.root / 'sys/class/block' / name
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(node)
        self.write('dev/'+name, 'fixture block-device path')
        return node

    def internal_fixture(self):
        for name in ('mmcblk0p2', 'mmcblk0p3'):
            node = self.partition('mmcblk0', name, usb=False)
            (node.parent/'device').mkdir(exist_ok=True)
            (node.parent/'device/type').write_text('MMC')
        self.mounts = {'/': '/dev/mmcblk0p3', '/boot': '/dev/mmcblk0p2'}
        (self.root/'sys/block/mmcblk0/ro').write_text('0')

    def rooted_path(self, value):
        value = str(value)
        return self.root / value.lstrip('/') if value.startswith('/') else Path(value)

    def command(self, args, **kwargs):
        args = list(args)
        self.calls.append(args)
        if args[0] == 'findmnt':
            result = self.mounts[args[-1]]
        elif args[0] == '/sbin/modinfo':
            result = self.selected[args[-1]]
        elif args[0] == '/usr/bin/duet-camera-check':
            if not self.compatible:
                raise subprocess.CalledProcessError(1, args)
            result = ''
        elif args[0] in ('/sbin/depmod', '/sbin/modprobe', 'udevadm'):
            result = ''
        else:
            raise AssertionError('Unexpected external command: '+repr(args))
        return subprocess.CompletedProcess(args, 0, stdout=result+'\n')

    def invoke(self, arguments=('--external-usb',), failure=None):
        self.calls = []
        original_state = self.state.read_text()
        with patch.object(activate, 'Path', side_effect=self.rooted_path), \
                patch.object(activate.os, 'geteuid', return_value=0), \
                patch.object(activate.subprocess, 'run', side_effect=self.command), \
                patch.object(sys, 'argv', [str(SCRIPT), *arguments]), \
                contextlib.redirect_stdout(io.StringIO()):
            if failure:
                with self.assertRaises(failure):
                    activate.main()
            else:
                activate.main()
        # Even failed SCP checks must never stop the processor themselves.
        self.assertEqual(self.state.read_text(), original_state)
        if failure:
            self.assertFalse(any(c[0] in ('/sbin/modprobe', 'udevadm') for c in self.calls), self.calls)

    def test_loaded_but_unbound_camera_is_refused(self):
        (self.root/'sys/bus/platform/devices/1a006000.camera/driver').unlink()
        with patch.object(activate,'Path',side_effect=self.rooted_path):
            with self.assertRaises(ValueError):activate.probed_camera_gate()

    def test_bound_camera_without_endpoint_is_refused(self):
        (self.root/'sys/class/video4linux/video0/name').write_text('Different device')
        with patch.object(activate,'Path',side_effect=self.rooted_path):
            with self.assertRaises(ValueError):activate.probed_camera_gate()

    def test_external_loads_all_selected_modules_and_only_p1_gets_opt_in(self):
        self.invoke()
        loaded = [c for c in self.calls if c[0] == '/sbin/modprobe']
        self.assertEqual(loaded, [['/sbin/modprobe', n] for n in activate.MODULES[:-1]] +
                         [['/sbin/modprobe', 'mt8183_p1', 'external_usb=1']])
        self.assertLess(max(i for i, c in enumerate(self.calls) if c[0] == '/sbin/modinfo'),
                        min(i for i, c in enumerate(self.calls) if c[0] == '/sbin/modprobe'))

    def test_every_emmc_disk_partition_and_boot_region_must_be_read_only(self):
        for name in ('mmcblk0', 'mmcblk0/mmcblk0p1', 'mmcblk0/mmcblk0p3',
                     'mmcblk0boot0', 'mmcblk0boot1'):
            with self.subTest(device=name):
                ro = self.root / 'sys/block' / name / 'ro'
                ro.write_text('0')
                self.invoke(failure=ValueError)
                ro.write_text('1')
                self.assertFalse(any(c[0] == '/usr/bin/duet-camera-check' for c in self.calls))

    def test_absent_emmc_is_refused(self):
        (self.root/'sys/block/mmcblk0/device/type').write_text('SD')
        self.invoke(failure=ValueError)

    def test_root_or_boot_on_non_usb_storage_is_refused(self):
        self.partition('internal', 'internal1', usb=False)
        for mount in ('/', '/boot'):
            with self.subTest(mount=mount):
                original = self.mounts[mount]
                self.mounts[mount] = '/dev/internal1'
                self.invoke(failure=ValueError)
                self.mounts[mount] = original

    def test_root_or_boot_moved_to_a_different_usb_is_refused(self):
        self.partition('sdb', 'sdb1', usb=True)
        for mount in ('/', '/boot'):
            with self.subTest(mount=mount):
                original = self.mounts[mount]
                self.mounts[mount] = '/dev/sdb1'
                self.invoke(failure=ValueError)
                self.mounts[mount] = original

    def test_non_block_mount_source_is_refused(self):
        self.mounts['/'] = 'tmpfs'
        self.invoke(failure=ValueError)

    def test_other_module_path_is_refused_before_any_module_load(self):
        self.selected['mt8183_p1'] = '/lib/modules/other/mt8183_p1.ko'
        self.invoke(failure=ValueError)

    def test_selected_module_symlink_is_refused_before_any_module_load(self):
        selected = Path(self.selected['mt8183_p1'])
        selected.unlink()
        selected.symlink_to(self.write('unrelated-module.ko', 'other module'))
        self.invoke(failure=ValueError)

    def test_busy_scp_is_refused_without_depmod_or_stop(self):
        self.state.write_text('running')
        self.invoke(failure=ValueError)
        self.assertFalse(any(c[0] == '/sbin/depmod' for c in self.calls))

    def test_incompatible_abi_is_refused_without_depmod(self):
        self.compatible = False
        self.invoke(failure=subprocess.CalledProcessError)
        self.assertFalse(any(c[0] == '/sbin/depmod' for c in self.calls))

    def test_external_mode_cannot_override_hardware_root(self):
        self.invoke(('--external-usb', '--hardware-root', '/proc/1/root'), failure=ValueError)
        self.assertEqual(self.calls, [])

    def test_internal_loads_only_p1_with_internal_opt_in(self):
        self.internal_fixture()
        self.invoke(('--internal-emmc',))
        self.assertEqual([c for c in self.calls if c[0] == '/sbin/modprobe'],
                         [['/sbin/modprobe', n] for n in activate.MODULES[:-1]] +
                         [['/sbin/modprobe', 'mt8183_p1', 'internal_emmc=1']])
        self.assertIn(['/usr/bin/duet-camera-check', '--root', '/'], self.calls)

    def test_internal_mode_cannot_override_hardware_root_or_combine_modes(self):
        self.invoke(('--internal-emmc', '--hardware-root', '/proc/1/root'), failure=ValueError)
        self.assertEqual(self.calls, [])
        with contextlib.redirect_stderr(io.StringIO()):
            self.invoke(('--internal-emmc', '--external-usb'), failure=SystemExit)
        self.assertEqual(self.calls, [])

    def test_internal_mode_requires_root(self):
        with patch.object(activate.os, 'geteuid', return_value=1000), \
                patch.object(activate.subprocess, 'run') as run, \
                patch.object(sys, 'argv', [str(SCRIPT), '--internal-emmc']):
            with self.assertRaisesRegex(ValueError, 'Run as root'):
                activate.main()
        run.assert_not_called()

    def test_internal_mode_refuses_usb_and_mixed_storage(self):
        self.invoke(('--internal-emmc',), failure=ValueError)
        self.internal_fixture()
        self.mounts['/boot'] = '/dev/sda2'
        self.invoke(('--internal-emmc',), failure=ValueError)

    def test_internal_mode_refuses_sd_cards_and_second_mmc(self):
        self.internal_fixture()
        disk = (self.root/'sys/class/block/mmcblk0p2').resolve().parent
        (disk/'device/type').write_text('SD')
        self.invoke(('--internal-emmc',), failure=ValueError)
        (disk/'device/type').write_text('MMC')
        node = self.partition('mmcblk1', 'mmcblk1p2', usb=False)
        (node.parent/'device').mkdir()
        (node.parent/'device/type').write_text('MMC')
        self.mounts['/boot'] = '/dev/mmcblk1p2'
        self.invoke(('--internal-emmc',), failure=ValueError)

    def test_internal_mode_refuses_nonpartition_and_usb_mmc(self):
        self.internal_fixture()
        node = (self.root/'sys/class/block/mmcblk0p2').resolve()
        (node/'partition').unlink()
        self.invoke(('--internal-emmc',), failure=ValueError)
        (self.root/'sys/class/block/mmcblk0p2').unlink()
        node = self.partition('mmcblk0', 'mmcblk0p2', usb=True)
        (node.parent/'device').mkdir()
        (node.parent/'device/type').write_text('MMC')
        self.invoke(('--internal-emmc',), failure=ValueError)

    def test_internal_mode_preserves_abi_and_offline_gates(self):
        self.internal_fixture()
        self.compatible = False
        self.invoke(('--internal-emmc',), failure=subprocess.CalledProcessError)
        self.compatible = True
        self.state.write_text('running')
        self.invoke(('--internal-emmc',), failure=ValueError)
        self.assertFalse(any(c[0] == '/sbin/depmod' for c in self.calls))

    def test_legacy_ram_defaults_and_hardware_root_keep_the_old_opt_in_behavior(self):
        # No qualifying USB or protected eMMC: the legacy RAM path must not
        # acquire external-storage permission implicitly.
        self.mounts = {'/': 'tmpfs', '/boot': 'tmpfs'}
        (self.root/'sys/block/mmcblk0/ro').write_text('0')
        for arguments, hardware_root in [((), '/'), (('--hardware-root', '/proc/1/root'), '/proc/1/root')]:
            with self.subTest(arguments=arguments):
                self.invoke(arguments)
                self.assertFalse(any(c[0] == 'findmnt' for c in self.calls))
                self.assertIn(['/usr/bin/duet-camera-check', '--root', hardware_root], self.calls)
                self.assertEqual([c for c in self.calls if c[0] == '/sbin/modprobe'],
                                 [['/sbin/modprobe', n] for n in activate.MODULES])


if __name__ == '__main__':
    unittest.main()
