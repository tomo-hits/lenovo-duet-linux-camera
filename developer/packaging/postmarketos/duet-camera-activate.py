#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Activate on an already compatible boot; never writes boot/FW/storage."""
from pathlib import Path
import argparse
import os
import re
import subprocess

KERNEL = '6.18.28-mt81'
MODULES = ('dw9768', 'ov02a10', 'ov8856', 'mtk_seninf', 'mt8183_p1')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def output(command):
    return subprocess.run(command, check=True, capture_output=True,
                          text=True).stdout.strip()


def external_usb_gate():
    parents = []
    for mount in ('/', '/boot'):
        source = output(['findmnt', '-n', '-o', 'SOURCE', '--mountpoint', mount])
        require(source.startswith('/dev/') and len(source.splitlines()) == 1,
                'External mode requires direct USB root and boot mounts')
        device = Path(source).resolve(strict=True)
        node = (Path('/sys/class/block') / device.name).resolve(strict=True)
        require((node / 'partition').is_file() and
                any(re.fullmatch(r'usb[0-9]+', part) for part in node.parts),
                'Root and boot must be USB partitions')
        parents.append(node.parent)
    require(parents[0] == parents[1], 'Root and boot must share one USB')

    found = False
    for disk in Path('/sys/block').glob('mmcblk[0-9]*'):
        kind = disk / 'device/type'
        if not kind.exists() or kind.read_text().strip() != 'MMC':
            continue
        found = True
        devices = [disk, *disk.glob(disk.name + 'p*'),
                   *disk.parent.glob(disk.name + 'boot*')]
        for device in devices:
            require((device / 'ro').read_text().strip() == '1',
                    'Internal eMMC is writable: ' + device.name)
    require(found, 'Protected internal eMMC was not found')


def internal_emmc_gate():
    """Admit only direct root/boot partitions on the same internal MMC.

    This explicit OS-install route does not change block protection, mount
    options, boot partitions, firmware, or the hardware compatibility checks.
    """
    parents = []
    for mount in ('/', '/boot'):
        source = output(['findmnt', '-n', '-o', 'SOURCE', '--mountpoint', mount])
        require(source.startswith('/dev/') and len(source.splitlines()) == 1,
                'Internal mode requires direct eMMC root and boot mounts')
        device = Path(source).resolve(strict=True)
        node = (Path('/sys/class/block') / device.name).resolve(strict=True)
        disk = node.parent
        require(re.fullmatch(r'mmcblk[0-9]+p[0-9]+', device.name) and
                node.name == device.name and
                re.fullmatch(r'mmcblk[0-9]+', disk.name) and
                device.name.startswith(disk.name + 'p') and
                (node / 'partition').is_file() and
                not any(re.fullmatch(r'usb[0-9]+', part) for part in node.parts),
                'Internal root and boot must be eMMC partitions')
        require((disk / 'device/type').read_text().strip() == 'MMC',
                'Internal root and boot must use MMC, not an SD card')
        parents.append(disk)
    require(parents[0] == parents[1], 'Root and boot must share one internal eMMC')


def selected_module_paths():
    for name in MODULES:
        expected = Path('/lib/modules') / KERNEL / 'extra/duet-camera' / (name + '.ko')
        selected = output(['/sbin/modinfo', '-n', name])
        require(selected == str(expected) and expected.is_file() and not expected.is_symlink(),
                'The matched camera module must be selected for ' + name + ': ' + selected)


def probed_camera_gate():
    driver=Path('/sys/bus/platform/devices/1a006000.camera/driver')
    require(driver.is_symlink() and driver.resolve().name=='mtk-cam-p1-raw',
            'P1 module loaded but camera probe failed; inspect the kernel log')
    nodes=list(Path('/sys/class/video4linux').glob('video*'))
    require(any((node/'name').read_text().strip()=='MT8183 P1 RAW' for node in nodes),
            'The probed camera has no video endpoint')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--hardware-root', default='/')
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--external-usb', action='store_true',
                       help='Explicitly enable a protected external USB test boot')
    modes.add_argument('--internal-emmc', action='store_true',
                       help='Explicitly enable an already compatible internal eMMC OS')
    args = parser.parse_args()
    require(os.geteuid() == 0, 'Run as root after closing all camera applications.')
    if args.external_usb:
        require(args.hardware_root == '/', 'External mode must check the running hardware')
        external_usb_gate()
    elif args.internal_emmc:
        require(args.hardware_root == '/', 'Internal mode must check the running hardware')
        internal_emmc_gate()
    subprocess.run(['/usr/bin/duet-camera-check', '--root', args.hardware_root], check=True)
    state = Path('/sys/class/remoteproc/remoteproc0/state').read_text().strip()
    require(state == 'offline',
            'SCP is busy. Close camera/codec applications; no remoteproc stop or forced unload was performed.')
    subprocess.run(['/sbin/depmod', '-a', KERNEL], check=True)
    # Validate the complete selection before loading any camera module.
    selected_module_paths()
    for name in MODULES:
        command = ['/sbin/modprobe', name]
        if name == 'mt8183_p1' and args.external_usb:
            command.append('external_usb=1')
        elif name == 'mt8183_p1' and args.internal_emmc:
            command.append('internal_emmc=1')
        subprocess.run(command, check=True)
    for name in ('media', 'video4linux', 'misc'):
        subprocess.run(['udevadm', 'trigger', '--action=add', '--subsystem-match=' + name], check=True)
    subprocess.run(['udevadm', 'settle', '--timeout=10'], check=True)
    probed_camera_gate()
    print('Modules activated. Start/restart your normal PipeWire/WirePlumber user session, then open a camera application.')


if __name__ == '__main__':
    main()
