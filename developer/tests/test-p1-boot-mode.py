#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the production P1 admission helper, without loading a module."""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] /
          'modules/mt8183-p1-public/mt8183_p1_continuous.c').read_text()
match = re.search(r'static int boot_mode_gate\(bool ram\)\n\{.*?\n\}', source, re.S)
if not match:
    raise RuntimeError('Production boot-mode gate not found')
if 'module_param(internal_emmc, bool, 0400);' not in source:
    raise RuntimeError('Internal mode must remain root-readable and runtime read-only')
if 'ret = boot_mode_gate(ram);\n\tif (ret)\n\t\treturn ret;' not in source:
    raise RuntimeError('DT validation must enforce the production boot-mode gate')
program = '''#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
static bool external_usb, internal_emmc;
''' + match[0] + '''
int main(void) {
    for (int bits = 0; bits < 8; ++bits) {
        bool ram = bits & 1;
        external_usb = bits & 2;
        internal_emmc = bits & 4;
        int expected = external_usb && internal_emmc ? -EINVAL : bits ? 0 : -EPERM;
        assert(boot_mode_gate(ram) == expected);
    }
    puts("PASS 8 production P1 boot-mode admission combinations");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='duet-p1-boot-mode-') as tmp:
    unit = Path(tmp)/'test.c'
    binary = Path(tmp)/'test'
    unit.write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(unit), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
