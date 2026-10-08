#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host-only entrypoint tests. All privilege/session/updater commands are mocked."""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('entrypoint', ROOT / 'lib/entrypoint.py')
entrypoint = importlib.util.module_from_spec(spec)
spec.loader.exec_module(entrypoint)


class EntryPoints(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='duet entrypoints ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        (self.root / 'developer/scripts').mkdir(parents=True)
        (self.root / 'developer/scripts/apply-update.py').touch()
        (self.root / 'packages').mkdir()
        (self.root / 'packages/packages.json').write_text('{}')
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(mock.patch.object(entrypoint, 'ROOT', self.root))
        self.stack.enter_context(mock.patch.dict(entrypoint.os.environ, {}, clear=True))
        self.uid = self.stack.enter_context(mock.patch.object(entrypoint.os, 'geteuid', return_value=0))
        self.stack.enter_context(mock.patch.object(entrypoint.os, 'getuid', return_value=1000))
        self.stack.enter_context(mock.patch.object(entrypoint.pwd, 'getpwuid', return_value=self.account('camera')))
        self.stack.enter_context(mock.patch.object(entrypoint.pwd, 'getpwnam', side_effect=self.account))
        self.which = self.stack.enter_context(mock.patch.object(entrypoint.shutil, 'which', return_value='/mock/command'))
        self.run = self.stack.enter_context(mock.patch.object(
            entrypoint.subprocess, 'run', return_value=SimpleNamespace(returncode=0)))
        self.stdout, self.stderr = io.StringIO(), io.StringIO()
        self.stack.enter_context(contextlib.redirect_stdout(self.stdout))
        self.stack.enter_context(contextlib.redirect_stderr(self.stderr))

    @staticmethod
    def account(name):
        uids = {'root': 0, 'camera': 1000, 'second': 1001}
        if name not in uids:
            raise KeyError(name)
        return SimpleNamespace(pw_name=name, pw_uid=uids[name])

    def test_install_uses_root_packages_and_existing_updater(self):
        self.assertEqual(entrypoint.main(['install', '--user', 'camera']), 0)
        self.assertEqual(self.run.call_args.args[0][1:], [
            str(self.root / 'developer/scripts/apply-update.py'), 'apply', '--packages',
            str(self.root / 'packages'), '--user=camera', '--internal-emmc'])
        self.assertIn('Restart normally', self.stdout.getvalue())
        self.assertEqual(self.run.call_count, 1)

    def test_sudo_desktop_user_is_inferred(self):
        entrypoint.os.environ['SUDO_USER'] = 'camera'
        self.assertEqual(entrypoint.main(['install']), 0)
        self.assertIn('--user=camera', self.run.call_args.args[0])
        self.assertEqual(self.run.call_count, 1)

    def test_nonroot_escalates_only_after_preflight(self):
        self.uid.return_value = 1000
        entrypoint.os.environ['SUDO_USER'] = 'second'
        self.run.return_value.returncode = 17
        self.assertEqual(entrypoint.main(['install']), 17)
        command = self.run.call_args.args[0]
        self.assertEqual(command[:2], ['sudo', '--'])
        self.assertIn('--user=camera', command)
        self.assertNotIn('--user=second', command)
        self.assertEqual(self.run.call_count, 1)

    def test_source_checkout_fails_before_sudo_or_updater(self):
        self.uid.return_value = 1000
        (self.root / 'packages/packages.json').unlink()
        self.assertEqual(entrypoint.main(['install']), 1)
        self.run.assert_not_called()
        self.assertIn('complete update kit', self.stderr.getvalue())

    def test_restore_uses_saved_backup_without_downloaded_packages(self):
        (self.root / 'packages/packages.json').unlink()
        self.assertEqual(entrypoint.main(['restore', '--user', 'camera']), 0)
        self.assertEqual(self.run.call_args.args[0][2], 'restore')
        self.assertIn('restored system', self.stdout.getvalue())

    def test_custom_package_path_with_spaces_stays_one_argument(self):
        custom = self.root / 'other packages'
        custom.mkdir()
        (custom / 'packages.json').write_text('{}')
        self.assertEqual(entrypoint.main(['install', '--user=camera', '--packages', str(custom)]), 0)
        command = self.run.call_args.args[0]
        self.assertEqual(command[command.index('--packages') + 1], str(custom))

    def test_check_requires_no_graphical_session(self):
        self.assertEqual(entrypoint.main(['install', '--check']), 0)
        self.assertEqual(self.run.call_args.args[0][2], 'check')
        self.assertEqual(self.run.call_count, 1)
        self.assertNotIn('Restart', self.stdout.getvalue())

    def test_bad_or_root_user_fails_before_updater(self):
        for name in ('missing', 'root', ''):
            with self.subTest(name=name):
                self.assertEqual(entrypoint.main(['install', '--user', name]), 1)
                self.run.assert_not_called()

    def session_calls(self, sessions):
        def fake_run(command, **kwargs):
            if command[:2] == ['loginctl', 'list-sessions']:
                return SimpleNamespace(stdout='\n'.join(f'{n} x' for n in sessions))
            if command[:2] == ['loginctl', 'show-session']:
                return SimpleNamespace(stdout=sessions[command[2]])
            return SimpleNamespace(returncode=0)
        self.run.side_effect = fake_run

    @staticmethod
    def session(name='camera', uid=1000, remote='no', kind='wayland', active='yes'):
        return f'Name={name}\nUser={uid}\nActive={active}\nType={kind}\nRemote={remote}\nClass=user\n'

    def test_root_infers_unique_local_graphical_user(self):
        self.session_calls({'1': self.session(), '2': self.session('second', 1001, kind='tty')})
        self.assertEqual(entrypoint.main(['install']), 0)
        self.assertIn('--user=camera', self.run.call_args.args[0])

    def test_ambiguous_remote_and_inactive_sessions_are_not_guessed(self):
        examples = [
            {},
            {'1': self.session(), '2': self.session('second', 1001)},
            {'1': self.session(remote='yes')},
            {'1': self.session(active='no')},
            {'1': self.session(uid=999)},
        ]
        for sessions in examples:
            with self.subTest(sessions=sessions):
                self.run.reset_mock()
                self.session_calls(sessions)
                self.assertEqual(entrypoint.main(['install']), 1)
                self.assertTrue(all(call.args[0][0] == 'loginctl' for call in self.run.call_args_list))

    def test_login_manager_failure_does_not_run_update(self):
        self.run.side_effect = subprocess.TimeoutExpired('loginctl', 5)
        self.assertEqual(entrypoint.main(['install']), 1)
        self.assertEqual(self.run.call_count, 1)

    def test_failure_preserves_status_and_prints_usable_restore_command(self):
        self.run.return_value.returncode = 27
        self.assertEqual(entrypoint.main(['install', '--user', 'camera']), 27)
        import shlex
        restore_line = self.stderr.getvalue().splitlines()[-1].strip()
        self.assertEqual(shlex.split(restore_line), [
            'sudo', 'sh', str(self.root / 'restore.sh'), '--user=camera', '--internal-emmc'])
        self.assertNotIn('Restart normally', self.stdout.getvalue())

    def test_restore_failure_keeps_backup_advice(self):
        self.run.return_value.returncode = 11
        self.assertEqual(entrypoint.main(['restore', '--user', 'camera']), 11)
        self.assertIn('/var/lib/duet-camera-update', self.stderr.getvalue())

    def test_signal_status_has_standard_shell_exit_code(self):
        self.run.return_value.returncode = -15
        self.assertEqual(entrypoint.main(['install', '--user', 'camera']), 143)
        self.uid.return_value = 1000
        self.assertEqual(entrypoint.main(['install']), 143)

    def test_explicit_developer_key_is_forwarded_and_never_inferred(self):
        key_hash = 'AB' * 32
        self.assertEqual(entrypoint.main(['install', '--user', 'camera',
                                         '--expected-key-sha256', key_hash]), 0)
        command = self.run.call_args.args[0]
        self.assertEqual(command[command.index('--expected-key-sha256') + 1], key_hash.lower())

    def test_default_internal_and_explicit_usb_are_forwarded(self):
        for entry, extra in (('install', []), ('install', ['--check']), ('restore', [])):
            with self.subTest(entry=entry, extra=extra):
                self.assertEqual(entrypoint.main([entry, '--user=camera', *extra]), 0)
                self.assertIn('--internal-emmc', self.run.call_args.args[0])
                self.assertEqual(entrypoint.main([entry, '--user=camera', '--external-usb', *extra]), 0)
                self.assertNotIn('--internal-emmc', self.run.call_args.args[0])
                self.assertEqual(entrypoint.main([entry, '--user=camera', '--internal-emmc', *extra]), 0)
                self.assertIn('--internal-emmc', self.run.call_args.args[0])

    def test_boot_preparation_needs_no_graphical_login(self):
        self.assertEqual(entrypoint.main(['install','--prepare-boot','--check']),0)
        self.assertEqual(self.run.call_args.args[0][1:], [str(self.root/'developer/scripts/boot-update.py'), 'check', '--packages',str(self.root/'packages')])
        self.assertEqual(entrypoint.main(['install','--prepare-boot']),0)
        self.assertEqual(self.run.call_args.args[0][2],'prepare')

    def test_sudo_and_failure_recovery_keep_internal_mode(self):
        self.uid.return_value = 1000
        self.assertEqual(entrypoint.main(['install', '--internal-emmc']), 0)
        self.assertEqual(self.run.call_args.args[0][:2], ['sudo', '--'])
        self.assertIn('--internal-emmc', self.run.call_args.args[0])
        self.uid.return_value = 0
        self.run.return_value.returncode = 1
        self.assertEqual(entrypoint.main(['install', '--user=camera', '--internal-emmc']), 1)
        import shlex
        restore = shlex.split(self.stderr.getvalue().splitlines()[-1])
        self.assertEqual(restore, ['sudo', 'sh', str(self.root/'restore.sh'), '--user=camera', '--internal-emmc'])

    def test_help_and_bad_arguments_never_execute_commands(self):
        for args, expected in ((['install', '--help'], 0), (['restore', '--help'], 0),
                               (['restore', '--check'], 2), (['install', '--user'], 2),
                               (['install', '--expected-key-sha256', 'bad'], 2),
                               (['install', '--expected-key-sha256', ''], 2)):
            with self.subTest(args=args), self.assertRaises(SystemExit) as result:
                entrypoint.main(args)
            self.assertEqual(result.exception.code, expected)
        self.run.assert_not_called()


if __name__ == '__main__':
    unittest.main()
