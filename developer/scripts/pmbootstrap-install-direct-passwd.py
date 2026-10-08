#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run pinned pmbootstrap's image installer without its plaintext password file.

Use a terminal: credentials go directly to chroot passwd, never through Python.
The preparation mode requires an already locked account and configures no login.
Only image/rootfs preparation is permitted; physical-disk modes are rejected.
Upstream files remain unchanged. Requires the exact documented pmbootstrap tree.
"""
from pathlib import Path
import argparse
import hashlib
import importlib
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--pmbootstrap-tree', required=True, type=Path)
p.add_argument('--prepare-locked-account', action='store_true')
p.add_argument('arguments', nargs=argparse.REMAINDER)
a = p.parse_args()
args = a.arguments[1:] if a.arguments[:1] == ['--'] else a.arguments
if 'install' not in args or any(x == 'flasher' or x.startswith(('--password', '--disk', '--sdcard', '--rsync')) for x in args):
    p.error('Only install to a file/rootfs is supported; password/disk/flasher arguments are forbidden')
if not a.prepare_locked_account and not sys.stdin.isatty():
    p.error('Use a local interactive terminal for passwd')
tree = a.pmbootstrap_tree.resolve()
expected = {
    'pmb/commands/install.py': '721ec3d18163dd9769ee7623865e203e50937252534bcd55d62c6a3b99904ea8',
    'pmb/install/_install.py': '166c3d8c12b548441935f479d0d08f68b1652ff255a9c2d4b8cbdc98beef19de',
}
for name, sha in expected.items():
    if hashlib.sha256((tree / name).read_bytes()).hexdigest() != sha:
        p.error('The unmodified pinned pmbootstrap installer source is required')
sys.path.insert(0, str(tree))
import pmb
import pmb.chroot
from pmb.types import RunOutputTypeDefault, PmbArgs
upstream_parser = importlib.import_module('pmb.parse.arguments').get_parser()
parsed = upstream_parser.parse_args(args, namespace=PmbArgs())
if parsed.action != 'install' or parsed.disk or parsed.rsync or parsed.password:
    p.error('Only install to a file/rootfs is supported')
command = importlib.import_module('pmb.commands.install')
implementation = importlib.import_module('pmb.install._install')

def login_direct(config, chroot, unused):
    if a.prepare_locked_account:
        status = pmb.chroot.root(['passwd', '-S', config.user], chroot, output_return=True)
        if status.split()[1] != 'L':
            raise RuntimeError('Preparation requires a locked account; credentials were not changed')
    else:
        pmb.chroot.root(['passwd', config.user], chroot, output=RunOutputTypeDefault.INTERACTIVE)
    pmb.chroot.root(['passwd', '-l', 'root'], chroot)

# Upstream calls getpass twice before rootfs preparation. No credential is
# collected here; the real passwd utility runs later on the prepared rootfs.
command.getpass = lambda prompt: 'not-a-login-credential'
implementation.setup_login = login_direct
sys.argv = [str(tree / 'pmbootstrap.py'), *args]
raise SystemExit(pmb.main())
