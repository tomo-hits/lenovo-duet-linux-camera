#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the actual hook on isolated filesystem fixtures, never real devices."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

HOOK = (Path(__file__).resolve().parent.parent / 'packaging/postmarketos/external-usb-guard.sh').read_text()


class GuardTests(unittest.TestCase):
    def run_guard(self, variant='ok'):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'sys/block').mkdir(parents=True)
            (root / 'sys/class/block').mkdir(parents=True)
            (root / 'dev').mkdir()
            (root / 'bin').mkdir()
            for name in ['mmcblk0', 'mmcblk0p3', 'mmcblk0boot0']:
                p = root / 'sys/block/mmcblk0' / name if name.endswith('p3') else root / 'sys/block' / name
                p.mkdir()
                (p / 'ro').write_text('0\n')
                (root / 'sys/class/block' / name).symlink_to(p)
                if name == 'mmcblk0':
                    (p / 'device').mkdir()
                    (p / 'device/type').write_text('SD\n' if variant == 'no_mmc' else 'MMC\n')
            # Realistic sysfs partition ancestors are required by the hook.
            for name, parent in [('sda2', 'sda'), ('sda3', 'sda')]:
                usb = 'platform' if variant == 'not_usb' else 'usb1/1-1'
                if variant == 'different_usb' and name == 'sda2':
                    parent = 'sdb'
                p = root / 'sys/devices' / usb / 'block' / parent / name
                p.mkdir(parents=True)
                (p / 'partition').write_text('2\n')
                (root / 'sys/class/block' / name).symlink_to(p)
            for name, value in [('uuid-root', 'sda3'), ('uuid-boot', 'sda2')]:
                entries = [str(root / 'dev' / value)]
                if variant == 'duplicate' and name == 'uuid-root':
                    entries *= 2
                if variant == 'missing_partition' and name == 'uuid-root':
                    entries = []
                (root / name).write_text('\n'.join(entries) + ('\n' if entries else ''))
            scripts = {
                'blkid': f'#!/bin/sh\nkey=${{2#UUID=}}\ncat "{root}/$key"\n',
                'blockdev': (f'#!/bin/sh\nname=${{2##*/}}\necho "$name" >> "{root}/protection-log"\n' +
                             ('exit 1\n' if variant == 'protect_failure' else
                              f'echo 1 > "{root}/sys/class/block/$name/ro"\n')),
                'busybox': '#!/usr/bin/env python3\nimport os,sys\nprint(os.path.realpath(sys.argv[-1]))\n',
                'sleep': '#!/bin/sh\nexit 0\n',
            }
            for name, data in scripts.items():
                p = root / 'bin' / name
                p.write_text(data)
                p.chmod(0o755)
            patched = HOOK.replace('while :; do sleep 3600; done', 'exit 73')
            patched = patched.replace('/sys/', f'{root}/sys/').replace('/dev/', f'{root}/dev/')
            patched = patched.replace('/sbin/blockdev', str(root / 'bin/blockdev'))
            patched = patched.replace('/bin/busybox', str(root / 'bin/busybox'))
            p = root / 'hook.sh'
            p.write_text(patched)
            env = dict(os.environ, PATH=str(root / 'bin') + ':' + os.environ['PATH'],
                       root_uuid='uuid-root', boot_uuid='uuid-boot', root_path='', boot_path='')
            if variant == 'missing_uuid':
                env['root_uuid'] = ''
            if variant == 'path_override':
                env['root_path'] = '/dev/mmcblk0p3'
            r = subprocess.run(['sh', str(p)], env=env, capture_output=True, text=True, timeout=5)
            if variant == 'ok':
                for name in ['mmcblk0', 'mmcblk0p3', 'mmcblk0boot0']:
                    self.assertEqual((root / 'sys/class/block' / name / 'ro').read_text().strip(), '1')
            return r

    def test_usb_and_mmc_protection(self):
        r = self.run_guard()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('DUET_EXTERNAL_GUARD_OK', r.stdout)

    def test_rejected_conditions(self):
        for variant in ['not_usb', 'different_usb', 'duplicate', 'missing_partition',
                        'missing_uuid', 'path_override', 'protect_failure', 'no_mmc']:
            with self.subTest(variant=variant):
                r = self.run_guard(variant)
                self.assertEqual(r.returncode, 73, r.stdout + r.stderr)
                self.assertIn('DUET_EXTERNAL_GUARD_FAILED', r.stderr)


if __name__ == '__main__':
    unittest.main()
