#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""User-facing entry points; all update and recovery checks stay in the updater."""
import argparse
from pathlib import Path
import os
import pwd
import re
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def desktop_user(name):
    try:
        account = pwd.getpwnam(name)
    except KeyError:
        raise ValueError(f'Unknown desktop user: {name}') from None
    if account.pw_uid == 0:
        raise ValueError('Select the desktop user with --user NAME; root is not a desktop user.')
    return account


def graphical_user():
    """Only accept a unique, active, local graphical login owned by a real user."""
    if not shutil.which('loginctl'):
        raise ValueError('Specify the desktop user with --user NAME.')
    sessions = subprocess.run(['loginctl', 'list-sessions', '--no-legend', '--no-pager'],
                              check=True, capture_output=True, text=True, timeout=5)
    candidates = set()
    for line in sessions.stdout.splitlines():
        if not line.strip():
            continue
        session = line.split()[0]
        result = subprocess.run(['loginctl', 'show-session', session, '--no-pager',
                                 '-p', 'Name', '-p', 'User', '-p', 'Active', '-p', 'Type',
                                 '-p', 'Remote', '-p', 'Class'], check=True,
                                capture_output=True, text=True, timeout=5)
        fields = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)
        if (fields.get('Active') == 'yes' and fields.get('Remote') == 'no'
                and fields.get('Type') in ('wayland', 'x11') and fields.get('Class') == 'user'):
            account = desktop_user(fields.get('Name', ''))
            if str(account.pw_uid) != fields.get('User'):
                raise ValueError('The graphical session user is inconsistent; use --user NAME.')
            candidates.add(account.pw_name)
    if len(candidates) != 1:
        raise ValueError('No unique active desktop user was found. Specify --user NAME.')
    return candidates.pop()


def select_user(explicit):
    if explicit is not None:
        return desktop_user(explicit).pw_name
    if os.geteuid() != 0:
        return desktop_user(pwd.getpwuid(os.getuid()).pw_name).pw_name
    sudo_user = os.environ.get('SUDO_USER')
    if sudo_user and sudo_user != 'root':
        return desktop_user(sudo_user).pw_name
    return graphical_user()


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] not in ('install', 'restore'):
        raise ValueError('Use install.sh or restore.sh.')
    entry = argv.pop(0)
    parser = argparse.ArgumentParser(
        prog=f'sudo sh {entry}.sh',
        description=('Check and install the matched camera update, saving the previous packages and settings.'
                     if entry == 'install' else 'Restore the saved packages and settings from the last update.'))
    parser.add_argument('--user', metavar='NAME', help='desktop user (normally detected automatically)')
    parser.add_argument('--internal-emmc', action='store_true',
                        help='select internal eMMC (the default)')
    parser.add_argument('--external-usb', action='store_true', help='advanced: select a prepared external USB OS')
    if entry == 'install':
        parser.add_argument('--prepare-boot', action='store_true', help='save and integrate the supported internal stock boot DT; restart and run install again')
    parser.add_argument('--packages', type=Path, metavar='PATH', default=ROOT / 'packages',
                        help=('signed package directory (default: packages beside install.sh)'
                              if entry == 'install' else 'accepted for symmetry; restore uses its saved backup'))
    if entry == 'install':
        parser.add_argument('--check', action='store_true', help='check compatibility and packages without installing')
        parser.add_argument('--expected-key-sha256', metavar='HEX',
                            help='developer builds: explicitly trusted signing public-key SHA-256')
    args = parser.parse_args(argv)
    action = 'check' if getattr(args, 'check', False) else ('apply' if entry == 'install' else 'restore')
    key_hash = getattr(args, 'expected_key_sha256', None)
    if key_hash is not None and not re.fullmatch(r'[0-9a-fA-F]{64}', key_hash):
        parser.error('--expected-key-sha256 must contain exactly 64 hexadecimal characters')
    key_args = ['--expected-key-sha256', key_hash.lower()] if key_hash else []
    if args.internal_emmc and args.external_usb:
        parser.error('Choose only one storage mode')
    if getattr(args, 'prepare_boot', False) and args.external_usb:
        parser.error('Boot preparation supports internal eMMC only')
    mode_args = [] if args.external_usb else ['--internal-emmc']
    updater = ROOT / 'developer/scripts/apply-update.py'
    try:
        if not updater.is_file():
            raise ValueError('The update kit is incomplete: developer/scripts/apply-update.py is missing.')
        package_dir = args.packages.resolve()
        if entry == 'install' and not (package_dir / 'packages.json').is_file():
            raise ValueError('Signed packages are missing. Download and extract the complete update kit, '
                             'or select its package directory with --packages PATH. A source checkout alone cannot install.')
        # A check does not stop a media session, so it needs no desktop identity.
        username = select_user(args.user) if (action != 'check' and not getattr(args, 'prepare_boot', False)) or args.user is not None else None
        user_args = ['--user=' + username] if username else []
        if os.geteuid() != 0:
            if not shutil.which('sudo'):
                raise ValueError(f'sudo is required. Ask an administrator to run: sudo sh {entry}.sh --user NAME')
            command = ['sudo', '--', sys.executable, str(Path(__file__).resolve()), entry,
                       '--packages', str(package_dir), *user_args, *key_args, *mode_args]
            if args.external_usb:
                command.append('--external-usb')
            if getattr(args, 'prepare_boot', False):
                command.append('--prepare-boot')
            if action == 'check':
                command.append('--check')
            status = subprocess.run(command).returncode
            return 128 - status if status < 0 else status
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        print(f'Cannot start: {error}', file=sys.stderr)
        return 1

    boot_updater = ROOT / 'developer/scripts/boot-update.py'
    if getattr(args, 'prepare_boot', False):
        return subprocess.run([sys.executable, str(boot_updater), 'check' if action == 'check' else 'prepare', '--packages', str(package_dir)]).returncode
    # A failed or boot-only installation can be restored without a package change.
    package_state = Path('/var/lib/duet-camera-update/latest/state.json')
    boot_state = Path('/var/lib/duet-camera-boot-preparation/latest.json')
    boot_only_restore = False
    if action == 'restore' and not args.external_usb and boot_state.exists():
        import json
        current = json.loads(package_state.read_text()) if package_state.exists() else None
        boot_only_restore = current is None or current.get('status') == 'RESTORED'
    try:
        result = subprocess.run([sys.executable, str(updater), action,
                                 '--packages', str(package_dir), *user_args, *key_args, *mode_args]) if not boot_only_restore else None
        status = result.returncode if result is not None else 0
        if status < 0:
            status = 128 - status
    except KeyboardInterrupt:
        status = 130
    except OSError as error:
        print(f'Cannot run the updater: {error}', file=sys.stderr)
        status = 1
    if status:
        print(f'{entry.capitalize() if action != "check" else "Check"} failed (exit {status}). '
              'Read the error above.', file=sys.stderr)
        if action == 'apply':
            restore = ['sudo', 'sh', str(ROOT / 'restore.sh'), *user_args, *(['--external-usb'] if args.external_usb else mode_args)]
            print('If the update changed the system, restore with:\n  ' + shlex.join(restore), file=sys.stderr)
        elif action == 'restore':
            print('Keep this update kit and /var/lib/duet-camera-update for recovery.', file=sys.stderr)
        return status
    if action == 'restore' and not args.external_usb and boot_state.exists():
        status = subprocess.run([sys.executable, str(boot_updater), 'restore']).returncode
        if status:
            print('Boot restoration failed. Keep all saved boot/package backups.', file=sys.stderr)
            return status
    if action != 'check':
        print('Restart normally to use the restored system.' if action == 'restore'
              else 'Restart normally, then open Snapshot to check the front and rear cameras.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
