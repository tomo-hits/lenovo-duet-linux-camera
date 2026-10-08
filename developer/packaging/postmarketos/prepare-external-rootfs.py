#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Prepare an overlay for an OFFLINE, fresh pmbootstrap rootfs.

Output only. Never installs files, calls mkinitfs, mounts/writes a device, or
changes the supplied rootfs. Real USB boot acceptance is still required.
"""
from pathlib import Path
import argparse
import gzip
import hashlib
import importlib
import json
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--rootfs', required=True, type=Path)
p.add_argument('--root-uuid', required=True)
p.add_argument('--boot-uuid', required=True)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--abi', type=Path, default=Path(__file__).with_name('kernel-abi.json'))
a = p.parse_args()
here = Path(__file__).resolve().parent
root, out = a.rootfs.resolve(), a.output.absolute()
abi = json.loads(a.abi.read_text())

def require(condition, reason):
    if not condition:
        raise ValueError(reason)

require(root != Path('/'), 'Supply the offline experimental rootfs, not /')
require(root.is_dir(), 'Offline rootfs missing')
require(not out.exists() and not out.is_symlink(), 'Output must be new')
require(not out.resolve().is_relative_to(root), 'Output must be outside the rootfs')
for value in (a.root_uuid, a.boot_uuid):
    require(re.fullmatch(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', value), 'Invalid filesystem UUID')
require(a.root_uuid != a.boot_uuid, 'Boot and root UUIDs must differ')
kernel = root / 'boot/vmlinuz'
require(kernel.is_file() and not kernel.is_symlink(), 'Expected packaged vmlinuz')
data = kernel.read_bytes()
image = gzip.decompress(data) if data.startswith(b'\x1f\x8b') else data
require(hashlib.sha256(image).hexdigest() == abi['image_sha256'], 'Kernel Image mismatch')
fw = root / 'lib/firmware/mediatek/mt8183/scp.img'
compressed = False
if fw.is_file():
    firmware = fw.read_bytes()
else:
    fw = fw.with_name(fw.name + '.zst')
    require(fw.is_file(), 'SCP firmware missing')
    firmware = subprocess.run(['zstd', '-d', '-q', '-c', '--', str(fw)],
                              check=True, capture_output=True).stdout
    compressed = True
require(hashlib.sha256(firmware).hexdigest() == abi['firmware_sha256'], 'SCP firmware mismatch')
stock = root / 'boot/dtbs/mediatek/mt8183-kukui-krane-sku176.dtb'
tree, audit = importlib.import_module('integrate-camera-dtb').integrate(
    stock.read_bytes(), json.loads((here / 'camera-nodes.json').read_text()))
deviceinfo = root / 'etc/deviceinfo'
original = deviceinfo.read_text() if deviceinfo.exists() else ''
relative_dtb = 'boot/dtbs/mediatek/duet-camera-sku176.dtb'
scp_init = importlib.import_module('external-scp-init')
files = {
    relative_dtb: tree,
    'etc/deviceinfo': (original.rstrip() + '\n# Duet SKU176 external camera experiment\n'
                       'deviceinfo_dtb="mediatek/duet-camera-sku176"\n').encode(),
    'etc/kernel-cmdline.d/90-duet-external.conf':
        f'pmos.root_uuid={a.root_uuid} pmos.boot_uuid={a.boot_uuid}\n'.encode(),
    'etc/mkinitfs/hooks/00-duet-external-guard.sh': (here / 'external-usb-guard.sh').read_bytes(),
    'etc/mkinitfs/files/00-duet-external-guard.files': b'/usr/sbin/blockdev:/sbin/blockdev\n',
    'etc/modprobe.d/duet-camera-first-install.conf':
        b'# Manual loading / activation service overrides these alias blacklists.\n'
        b'blacklist ov02a10\nblacklist ov8856\nblacklist dw9768\n'
        b'blacklist mtk_seninf\nblacklist mt8183_p1\n',
    'etc/modprobe.d/duet-camera-scp-isolation.conf': scp_init.CONFIG.encode(),
    'usr/libexec/duet-camera-external-scp-init': (here / 'external-scp-init.py').read_bytes(),
    'usr/lib/systemd/system/duet-camera-scp-init.service':
        b'[Unit]\nDescription=Initialize isolated external Duet SCP once\n'
        b'After=systemd-udev-trigger.service\nBefore=display-manager.service duet-camera-activate.service\n'
        b'\n[Service]\nType=oneshot\nExecStart=/usr/libexec/duet-camera-external-scp-init\n'
        b'TimeoutStartSec=20\nRemainAfterExit=yes\n'
        b'\n[Install]\nWantedBy=multi-user.target\n',
    'etc/systemd/system/duet-camera-activate.service.d/10-scp-init.conf':
        b'[Unit]\nRequires=duet-camera-scp-init.service\nAfter=duet-camera-scp-init.service\n',
    'usr/lib/systemd/system/duet-camera-activate.service':
        b'[Unit]\nDescription=Activate matched Duet camera modules\n'
        b'ConditionPathExists=/usr/bin/duet-camera-activate\n'
        b'After=systemd-udev-trigger.service\nBefore=display-manager.service\n'
        b'\n[Service]\nType=oneshot\nExecStart=/usr/bin/duet-camera-activate --external-usb\nRemainAfterExit=yes\n'
        b'\n[Install]\nWantedBy=multi-user.target\n',
}
for filename in ('duet-camera-check.py', 'kernel-abi.json', 'camera-nodes.json',
                 'fdt.py', 'integrate-camera-dtb.py', 'legacy_bindings.py'):
    files['usr/libexec/duet-camera-external-check/' + filename] = (here / filename).read_bytes()
for name in files:
    if name != 'etc/deviceinfo':
        require(not (root / name).exists() and not (root / name).is_symlink(),
                f'Existing project overlay file in input rootfs: {name}')
out.mkdir(parents=True)
for name, content in files.items():
    dest = out / 'overlay' / name
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(content)
    dest.chmod(0o755 if name.endswith('.sh') or name == 'usr/libexec/duet-camera-external-scp-init' else 0o644)
link = out / 'overlay/etc/systemd/system/multi-user.target.wants/duet-camera-activate.service'
link.parent.mkdir(parents=True, exist_ok=True)
link.symlink_to('/usr/lib/systemd/system/duet-camera-activate.service')
link = out / 'overlay/etc/systemd/system/multi-user.target.wants/duet-camera-scp-init.service'
link.symlink_to('/usr/lib/systemd/system/duet-camera-scp-init.service')
record = {
    'kernel': abi['kernel'], 'image_sha256': abi['image_sha256'],
    'firmware_sha256': abi['firmware_sha256'], 'compressed_firmware': compressed,
    'root_uuid': a.root_uuid, 'boot_uuid': a.boot_uuid, 'dt_audit': audit,
    'files_sha256': {name: hashlib.sha256(content).hexdigest() for name, content in files.items()},
    'supplied_rootfs_modified': False, 'boot_or_media_written': False,
    'physical_boot_accepted': False,
    'SCP_isolation': 'codec/MDP/JPEG autoload blocked; cold-boot provider registration followed by one auto-boot-reference shutdown',
    'next': 'Review/copy overlay into the isolated rootfs; rebuild mkinitfs and the image; verify FIT/GPT/filesystems before any media write.',
}
(out / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record))
