#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Cold-boot-only SCP initialization for an explicitly selected camera OS.

The matched upstream driver auto-boots once on registration. Block all other
SCP consumers before boot, explicitly load the provider, then return only that
single auto-boot reference. Never drain refs, unload, or retry on a live boot.
"""
from pathlib import Path
import argparse, json, os, re, subprocess, time

CONSUMERS = ('mtk_mdp3', 'mtk_vcodec_common', 'mtk_vcodec_dec', 'mtk_vcodec_enc',
             'mtk_jpeg', 'mt8183_p1', 'duet_p1_video_raw')
BLOCKED = CONSUMERS[:5]
CONFIG = '# Dedicated external camera OS: codec/MDP coexistence is untested.\n' + ''.join(
    f'blacklist {n}\ninstall {n} /bin/false\n' for n in (*BLOCKED, 'mtk_scp'))
INTERNAL_CONFIG = CONFIG.replace('# Dedicated external camera OS:',
                                 '# Explicit internal eMMC camera mode:')

def require(ok, why):
    if not ok:
        raise ValueError(why)

def check_clients(root=Path('/')):
    for n in CONSUMERS:
        require(not (root / 'sys/module' / n).exists(), 'SCP consumer loaded: ' + n)
    holders = root / 'sys/module/mtk_scp/holders'
    if holders.exists():
        require(not list(holders.iterdir()), 'SCP has module holders')

def internal_emmc_gate():
    parents = []
    for mount in ('/', '/boot'):
        source = subprocess.check_output(
            ['findmnt', '-n', '-o', 'SOURCE', '--mountpoint', mount], text=True).strip()
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

def external_usb_gate():
    disks = []
    for p in Path('/sys/block').glob('mmcblk[0-9]*'):
        kind = p / 'device/type'
        if kind.exists() and kind.read_text().strip() == 'MMC':
            disks.append(p)
            for q in [p, *p.glob(p.name+'p*'), *p.parent.glob(p.name+'boot*')]:
                require((q/'ro').read_text().strip() == '1', 'eMMC is writable: '+q.name)
    require(bool(disks), 'Protected eMMC required')
    parents = []
    for mount in ('/', '/boot'):
        dev = subprocess.check_output(['findmnt', '-n', '-o', 'SOURCE', mount], text=True).strip()
        require(dev.startswith('/dev/'), 'Direct USB block mount required')
        p = (Path('/sys/class/block') / Path(dev).resolve().name).resolve()
        require('/usb' in str(p) and (p/'partition').exists(), 'USB partition required')
        parents.append(p.parent)
    require(parents[0] == parents[1], 'Boot/root must be on the same USB')

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--internal-emmc', action='store_true',
                        help='Explicitly select an already compatible internal eMMC OS')
    args = parser.parse_args(argv)
    require(os.geteuid() == 0, 'Root required')
    expected_config = INTERNAL_CONFIG if args.internal_emmc else CONFIG
    require(Path('/etc/modprobe.d/duet-camera-scp-isolation.conf').read_text() == expected_config,
            'Exact cold-boot isolation configuration required')
    require(not Path('/sys/module/mtk_scp').exists(), 'Provider already loaded; cold boot required')
    check_clients()
    if args.internal_emmc:
        internal_emmc_gate()
    else:
        external_usb_gate()
    kit = Path('/usr/libexec/duet-camera-external-check')
    checker = Path('/usr/bin/duet-camera-check')
    command = [str(checker)] if checker.exists() else ['python3', str(kit/'duet-camera-check.py'), '--abi', str(kit/'kernel-abi.json')]
    result = json.loads(subprocess.check_output(command, text=True))
    require(result['compatible'] is True, 'Exact kernel/DT/firmware compatibility required')
    boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    record = {'boot_id': boot, 'status': 'ATTEMPT', 'stop_requests': 0,
              'mode': 'internal-emmc' if args.internal_emmc else 'external-usb'}
    stamp = Path('/run/duet-camera-scp-init-attempt.json')
    with stamp.open('x') as f:
        json.dump(record, f)
    subprocess.run(['modprobe', '--ignore-install', 'mtk_scp'], check=True)
    state = Path('/sys/class/remoteproc/remoteproc0/state')
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if state.exists() and state.read_text().strip() == 'running':
            break
        time.sleep(.1)
    else:
        raise ValueError('Provider auto-boot did not complete')
    require(state.with_name('name').read_text().strip() == 'scp', 'Unexpected remote processor')
    check_clients()
    require(state.read_text().strip() == 'running', 'SCP state changed')
    # Exactly one write, owned by this cold-boot initialization. No stop loop.
    record['stop_requests'] = 1
    stamp.write_text(json.dumps(record))
    state.write_text('stop\n')
    require(state.read_text().strip() == 'offline', 'SCP did not stop after one request; refuse activation')
    check_clients()
    # The camera owns each SCP boot/shutdown reference. Automatic crash
    # recovery must not restart firmware while camera allocations are held.
    recovery = state.with_name('recovery')
    require(recovery.is_file(), 'SCP recovery control is missing')
    mode = recovery.read_text().strip()
    require(mode in ('enabled', 'disabled'), 'Unexpected SCP recovery mode')
    if mode == 'enabled':
        recovery.write_text('disabled\n')
    require(recovery.read_text().strip() == 'disabled',
            'SCP automatic recovery must be disabled before camera activation')
    require(state.read_text().strip() == 'offline', 'SCP state changed during initialization')
    record['recovery_disabled'] = True
    record['status'] = 'PASS'
    stamp.write_text(json.dumps(record))
    print(json.dumps(record))

if __name__ == '__main__':
    main()
