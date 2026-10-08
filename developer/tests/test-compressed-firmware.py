#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the real compatibility CLI against synthetic firmware and hardware files.

Usage: python3 tests/test-compressed-firmware.py COMPILED_STOCK_SKU176_DTB
Requires zstd. No real hardware paths, firmware files or images are modified.
"""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import tempfile

kit = Path(__file__).resolve().parent.parent
helpers = kit / 'packaging/postmarketos'
stock = Path(sys.argv[1]).resolve()
payload = b'benign synthetic firmware fixture, not executable firmware\n'
packed = subprocess.run(['zstd', '-q', '-c'], input=payload, capture_output=True, check=True).stdout
cases = ['raw', 'compressed', 'raw_priority', 'wrong_content', 'malformed', 'missing', 'missing_decoder']
count = 0
for flags in ([], ['-O']):
    with tempfile.TemporaryDirectory(prefix='duet-compressed-firmware-') as folder:
        tmp = Path(folder)
        dtb = tmp / 'integrated.dtb'
        subprocess.run([sys.executable, str(helpers / 'integrate-camera-dtb.py'), str(stock), str(dtb)],
                       capture_output=True, check=True)
        for case in cases:
            root = tmp / case
            for name, data in {
                'proc/sys/kernel/osrelease': b'6.18.28-mt81\n',
                'sys/kernel/notes': b'synthetic kernel notes',
                'sys/firmware/fdt': dtb.read_bytes(),
            }.items():
                p = root / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(data)
            fw = root / 'lib/firmware/mediatek/mt8183/scp.img'
            fw.parent.mkdir(parents=True)
            if case in ['raw', 'raw_priority']:
                fw.write_bytes(payload)
            if case in ['compressed', 'wrong_content', 'missing_decoder']:
                fw.with_suffix('.img.zst').write_bytes(packed)
            if case in ['malformed', 'raw_priority']:
                fw.with_suffix('.img.zst').write_bytes(b'not a zstd stream')
            abi = root / 'abi.json'
            abi.write_text(json.dumps({
                'kernel': '6.18.28-mt81',
                'kernel_notes_sha256': hashlib.sha256(b'synthetic kernel notes').hexdigest(),
                'compatible': 'google,krane-sku176',
                'firmware_sha256': hashlib.sha256(payload if case != 'wrong_content' else b'different').hexdigest(),
            }))
            before = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in root.rglob('*') if p.is_file()}
            env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
            if case == 'missing_decoder':
                env['PATH'] = str(tmp / 'empty-path')
            r = subprocess.run([sys.executable, *flags, str(helpers / 'duet-camera-check.py'),
                                '--root', str(root), '--abi', str(abi)],
                               capture_output=True, text=True, env=env, check=False)
            result = json.loads(r.stdout)
            success = case in ['raw', 'compressed', 'raw_priority']
            if result['compatible'] != success or (r.returncode == 0) != success:
                raise RuntimeError(f'{flags} {case}: {r.stdout} {r.stderr}')
            if success and result['observed']['firmware_compressed'] != (case == 'compressed'):
                raise RuntimeError(f'Wrong storage type: {case}')
            after = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in root.rglob('*') if p.is_file()}
            if before != after:
                raise RuntimeError(f'Compatibility check changed fixture files: {case}')
            count += 1
print(json.dumps({'cases_passed': count, 'hardware_test': False, 'files_unchanged': True}))
