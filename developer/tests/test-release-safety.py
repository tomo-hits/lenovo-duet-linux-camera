#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Duet camera project contributors
"""Exercise production CLIs in normal and optimized Python without hardware.

Supply a compiled stock SKU176 DTB. Synthetic Image bytes test the hash/input
contract only; this is not a kernel-image or actual boot acceptance test.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

kit = Path(__file__).resolve().parent.parent
helpers = kit / 'packaging/postmarketos'
stock = Path(sys.argv[1]).resolve()


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def invoke(flags, script, arguments, success, message=None):
    result = subprocess.run([sys.executable, *flags, str(script), *map(str, arguments)],
                            capture_output=True, text=True,
                            env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'))
    check((result.returncode == 0) == success, 'Unexpected CLI result: ' + script.name)
    if message:
        check(message in result.stderr, 'Wrong failure reason: ' + script.name)
    return result


results = []
for flags in ([], ['-O']):
    with tempfile.TemporaryDirectory(prefix='duet-release-safety-') as folder:
        tmp = Path(folder)
        image = tmp / 'Image'
        image.write_bytes(b'synthetic hash-contract fixture; not a kernel Image')
        abi = tmp / 'abi.json'
        abi.write_text(json.dumps({'image_sha256': hashlib.sha256(image.read_bytes()).hexdigest()}))
        producer = helpers / 'prepare-boot-assets.py'
        out = tmp / 'boot-assets'
        common = ['--base-dtb', stock, '--image', image, '--output', out]
        # Source-kit execution finds its own ABI even before any APK is installed.
        invoke(flags, producer, common, False, 'kernel asset mismatch')
        check(not out.exists(), 'Wrong Image created output')
        invoke(flags, producer, [*common, '--abi', abi], True)
        check((out / 'Image').read_bytes() == image.read_bytes(), 'Image copy changed')
        sentinel = out / 'sentinel'
        sentinel.write_text('preserve')
        invoke(flags, producer, [*common, '--abi', abi], False, 'output must be a new directory')
        check(sentinel.read_text() == 'preserve', 'Existing output changed')
        dangling = tmp / 'dangling-output'
        dangling.symlink_to(tmp / 'absent-target')
        invoke(flags, producer,
               ['--base-dtb', stock, '--image', image, '--output', dangling, '--abi', abi], False)
        check(not (tmp / 'absent-target').exists(), 'Dangling output followed')
        link = tmp / 'Image-link'
        link.symlink_to(image)
        invoke(flags, producer,
               ['--base-dtb', stock, '--image', link, '--output', tmp / 'link-output', '--abi', abi], False)
        protected = tmp / 'existing.dtb'
        protected.write_bytes(b'preserve existing file')
        invoke(flags, helpers / 'integrate-camera-dtb.py', [stock, protected], False)
        check(protected.read_bytes() == b'preserve existing file', 'Existing DT overwritten')
        invoke(flags, helpers / 'integrate-camera-dtb.py', [stock, dangling], False)
        check(not (tmp / 'absent-target').exists(), 'DT output symlink followed')
        # An invalid Image must also be refused by the real export auditor under -O.
        config, system_map = tmp / 'config', tmp / 'System.map'
        config.write_text('CONFIG_ARM64=y\n# CONFIG_MODVERSIONS is not set\n')
        system_map.write_text('')
        invoke(flags, kit / 'scripts/audit-kernel-exports.py',
               ['--image', image, '--config', config, '--system-map', system_map,
                '--output-dir', tmp / 'exports'], False, 'Not an uncompressed arm64 Image')
        check(not (tmp / 'exports').exists(), 'Invalid export input produced output')
        # Exercise the actual embedded build verifier against a changed source.
        source_dir = tmp / 'source'
        source_dir.mkdir()
        (source_dir / 'changed.cpp').write_text('modified')
        manifest = tmp / 'hashes.json'
        manifest.write_text(json.dumps({'files': {'changed.cpp': '0' * 64}}))
        script = (kit / 'scripts/build-userspace.sh').read_text()
        verifier = script.split("<<'PYSOURCE'\n", 1)[1].split('\nPYSOURCE', 1)[0]
        result = subprocess.run([sys.executable, *flags, '-c', verifier, str(source_dir), str(manifest)],
                                capture_output=True, text=True)
        check(result.returncode != 0 and 'libcamera source hash mismatch' in result.stderr,
              'Changed userspace source accepted')
        results.append({'optimized': bool(flags), 'cli_cases': 9, 'source_hash_rejection': True})
print(json.dumps({'status': 'PASS', 'modes': results, 'hardware_touched': False}))
