#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile public driver functions against host-only VB2 and protocol providers.

Use CC to choose the compiler. ASan/UBSan are enabled; this harness does not
establish real DMA synchronization, concurrent execution or recording FPS.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile


def function(source, name):
    match = re.search(r'^(?:static )?(?:int|void)\s+' + re.escape(name) + r'\(', source, re.M)
    if not match:
        raise RuntimeError(f'Production function not found: {name}')
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def main():
    root = Path(__file__).resolve().parents[1]
    tests = root / 'tests'
    front = (root / 'camera_video.c').read_text()
    header = '\n'.join(line for line in (root / 'camera_video.h').read_text().splitlines()
                       if not line.startswith('#include'))
    unit = '#include "delivery-shim.h"\n' + header + '\n'
    unit += '\n'.join(function(front, name) for name in
                      ['dcv_get_stats', 'queue_buffer', 'return_buffers', 'start', 'stop', 'dcv_deliver'])
    unit += '\n' + (tests / 'test_delivery.c').read_text().replace('int main(void)', 'static int frontend_main(void)')
    backend = (root / 'mt8183_p1_hw.c').read_text()
    backend_header = (root / 'mt8183_p1_hw.h').read_text()
    start = backend_header.index('struct mt8183_p1_frame_log {')
    end = backend_header.index('};', start) + 2
    unit += '\n' + backend_header[start:end]
    unit += '\n#include "delivery-backend-shim.h"\n'
    unit += '\n'.join(function(backend, name) for name in ['read_image', 'mt8183_p1_hw_publish_image'])
    unit += '\n' + (tests / 'test_delivery_backend.c').read_text()
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    with tempfile.TemporaryDirectory(prefix='mt8183-p1-delivery-') as temp:
        folder = Path(temp)
        source, executable = folder / 'actual.c', folder / 'check'
        source.write_text(unit)
        subprocess.run(compiler + ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                   '-Wno-misleading-indentation',  # Existing compact kernel source.
                                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                                   '-I' + str(tests), str(source), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == '__main__':
    main()
