#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build a NEW ChromeOS image file; physical devices are never valid inputs.

Linux/root, losetup, sfdisk, mount, e2fsck and a prepared overlay are required.
The source image must be offline. The source is never mounted or modified.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--image', type=Path, required=True)
p.add_argument('--prepared', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
source, prepared, out = a.image.absolute(), a.prepared.resolve(), a.output.absolute()

def require(condition, reason):
    if not condition:
        raise ValueError(reason)

def run(args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, **kwargs)

def unmount_all(mounted):
    """Keep an exact list of mounts still owned, even after partial failure."""
    errors = []
    for mountpoint in reversed(mounted.copy()):
        try:
            run(['umount', mountpoint])
        except Exception as error:
            errors.append(error)
        else:
            mounted.remove(mountpoint)
    return errors

def cleanup_message(errors):
    return 'Image cleanup failed:\n' + '\n'.join(str(error) for error in errors)

require(stat.S_ISREG(source.lstat().st_mode), 'Source must be a regular image file, never a device/symlink')
require(not out.exists() and not out.is_symlink(), 'Output must be new')
for suffix in ('.build', '.validation.json'):
    companion = Path(str(out) + suffix)
    require(not companion.exists() and not companion.is_symlink(), 'Output companions must be new')
manifest = json.loads((prepared / 'manifest.json').read_text())
overlay = prepared / 'overlay'
unit_links = {'etc/systemd/system/multi-user.target.wants/' + name:
              '/usr/lib/systemd/system/' + name for name in
              ('duet-camera-activate.service', 'duet-camera-scp-init.service')}
regular = set()
links = set()
for src in overlay.rglob('*'):
    name = str(src.relative_to(overlay))
    if src.is_symlink():
        require(name in unit_links and os.readlink(src) == unit_links[name], 'Unexpected overlay symlink')
        links.add(name)
    elif src.is_file():
        regular.add(name)
    else:
        require(src.is_dir(), 'Unexpected overlay file type')
require(regular == set(manifest['files_sha256']), 'Overlay file set mismatch')
require(links == set(unit_links), 'Activation/init link is missing')
for name, sha in manifest['files_sha256'].items():
    path = Path(name)
    require(not path.is_absolute() and '..' not in path.parts, 'Invalid overlay path')
    src = overlay / path
    require(src.is_file() and not src.is_symlink(), 'Expected regular overlay input')
    require(hashlib.sha256(src.read_bytes()).hexdigest() == sha, 'Overlay input hash mismatch')
require(os.geteuid() == 0, 'Run from a local administrator terminal')
loops = json.loads(run(['losetup', '--json', '--list'], capture_output=True, text=True).stdout)['loopdevices']
require(not any(x.get('back-file') and Path(x['back-file']).resolve() == source.resolve() for x in loops), 'Source image must be offline')
out.parent.mkdir(parents=True, exist_ok=True)
# Exclusive creation prevents accidental replacement of a previous image.
with source.open('rb') as src, out.open('xb') as dst:
    out.chmod(0o600)
    shutil.copyfileobj(src, dst, 1024 * 1024)
root = Path(str(out) + '.build')
root.mkdir()
loop = run(['losetup', '--find', '--show', '--partscan', out], capture_output=True, text=True).stdout.strip()
mounted = []
try:
    require(re.fullmatch(r'/dev/loop[0-9]+', loop), 'Not a loop device')
    backing = Path('/sys/block', Path(loop).name, 'loop/backing_file').read_text().strip()
    require(Path('/' + backing.lstrip('/')).resolve() == out.resolve(), 'Loop backing file mismatch')
    layout = json.loads(run(['sfdisk', '--json', loop], capture_output=True, text=True).stdout)['partitiontable']
    parts = layout['partitions']
    require(layout['label'] == 'gpt' and layout['sectorsize'] == 512 and len(parts) == 3, 'Unsupported image layout')
    require(parts[0]['node'] == loop + 'p1' and parts[0]['start'] == 8192 and
            parts[0]['type'].lower() == 'fe3a2a5d-4f32-41a7-b725-accc3285a309', 'Expected ChromeOS kernel partition')
    require(parts[1]['node'] == loop + 'p2' and parts[2]['node'] == loop + 'p3', 'Unexpected boot/root partitions')
    for index, key in [(1, 'boot_uuid'), (2, 'root_uuid')]:
        uuid = run(['blkid', '-s', 'UUID', '-o', 'value', parts[index]['node']], capture_output=True, text=True).stdout.strip()
        require(uuid == manifest[key], f'{key} differs from prepared overlay')
    run(['mount', loop + 'p3', root]); mounted.append(root)
    run(['mount', loop + 'p2', root / 'boot']); mounted.append(root / 'boot')
    for name in [*manifest['files_sha256'], *unit_links]:
        target = root / name
        for parent in target.parents:
            if parent == root:
                break
            require(not parent.is_symlink(), 'Destination parent is a symlink')
        if target.is_symlink():
            require(name in unit_links and os.readlink(target) == unit_links[name], 'Unexpected destination symlink')
    run(['cp', '-a', str(overlay) + '/.', root])
    for name in ['proc', 'sys', 'dev']:
        (root / name).mkdir(exist_ok=True)
    for name, kind, options in [('proc', 'proc', 'ro,nosuid,nodev,noexec'),
                                 ('sys', 'sysfs', 'ro,nosuid,nodev,noexec'),
                                 ('dev', 'tmpfs', 'nosuid,noexec')]:
        run(['mount', '-t', kind, '-o', options, kind, root / name]); mounted.append(root / name)
    for name, major, minor in [('null', 1, 3), ('zero', 1, 5), ('random', 1, 8), ('urandom', 1, 9), ('tty', 5, 0)]:
        run(['mknod', root / 'dev' / name, 'c', major, minor])
        (root / 'dev' / name).chmod(0o666)
    # No block nodes are exposed in the chroot. boot-deploy also detects chroot.
    run(['chroot', root, 'mkinitfs'])
    run(['chroot', root, 'vbutil_kernel', '--verify', '/boot/vmlinuz.kpart', '--verbose'])
    kpart = (root / 'boot/vmlinuz.kpart').read_bytes()
    require(0 < len(kpart) <= parts[0]['size'] * 512, 'Kernel image exceeds partition')
    with out.open('r+b') as f:
        f.seek(parts[0]['start'] * 512)
        f.write(kpart)
        f.flush()
        os.fsync(f.fileno())
    run(['sync'])
    errors = unmount_all(mounted)
    if errors:
        raise RuntimeError(cleanup_message(errors)) from errors[0]
    run(['e2fsck', '-fn', loop + 'p2'])
    run(['e2fsck', '-fn', loop + 'p3'])
    result = {
        'kernel_partition_size': len(kpart), 'kernel_partition_sha256': hashlib.sha256(kpart).hexdigest(),
        'root_uuid': manifest['root_uuid'], 'boot_uuid': manifest['boot_uuid'],
        'signature_and_filesystem_checks': 'PASS', 'source_image_modified': False,
        'physical_media_written': False, 'actual_boot_tested': False,
    }
finally:
    original_error = sys.exc_info()[1]
    errors = unmount_all(mounted)
    # A failed unmount must not prevent unrelated cleanup, or detach a loop
    # that still has mounted filesystems. Never retry a successful unmount.
    if mounted:
        errors.append(RuntimeError('Loop retained while mounts remain: ' +
                                   loop + ': ' + ', '.join(map(str, mounted))))
    elif re.fullmatch(r'/dev/loop[0-9]+', loop):
        try:
            run(['losetup', '-d', loop])
        except Exception as error:
            errors.append(error)
    if errors:
        if original_error is not None:
            # Preserve the build failure and its traceback; report cleanup
            # failures as additional diagnostics instead of replacing it.
            print(cleanup_message(errors), file=sys.stderr)
        else:
            raise RuntimeError(cleanup_message(errors)) from errors[0]

# Success evidence is emitted only after the loop device is detached too.
Path(str(out) + '.validation.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result))
