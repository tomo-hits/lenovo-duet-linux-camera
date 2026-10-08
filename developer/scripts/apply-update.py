#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Apply or restore one matched camera update on a prepared Duet OS.

Uses signed APK transactions and preserves the previous APKs before changing
the system. External USB is the default; internal eMMC OS changes require an
explicit --internal-emmc opt-in. Never writes boot assets or firmware. Reboot
is an explicit operator step; loaded drivers are never forcibly removed.
"""
from pathlib import Path
import argparse, base64, contextlib, datetime, fcntl, hashlib, importlib.util
import json, os, pwd, re, shutil, stat, subprocess, tempfile
from urllib.parse import unquote, urlparse
from urllib.request import urlopen

KIT = Path(__file__).resolve().parent.parent
STATE = Path('/var/lib/duet-camera-update')
KEY_HASH = '94d05c05d71e63aa74b0a2f11a4f4e3d8138e701daf5fe4f95e980b8fef73cd9'
PINS = {'duet-camera': '0.2.13-r1', 'duet-camera-modules': '0.2.8-r0',
        'duet-camera-config': '0.1.3-r1', 'libcamera': '0.7.2-r103',
        'libcamera-ipa': '0.7.2-r103', 'libcamera-tools': '0.7.2-r103',
        'pipewire-libs': '1.6.8-r104', 'gst-plugin-pipewire': '1.6.8-r104'}
OFFICIAL_ZSTD = {
    'version': '1.5.7-r2',
    'apk_sha256': '0e4d6b9bcb7db12e3fcd580c6226a88884ddfa561ebc7f64285baef5e789b541',
    'index_sha256': 'fd05c717626c3de4c171f325d6b400397155d04713059b4a0d1ca8e11293d4a5',
    'key_name': 'alpine-devel@lists.alpinelinux.org-616ae350.rsa.pub',
    'key_sha256': 'd11f6b21c61b4274e182eb888883a8ba8acdbf820dcc7a6d82a7d9fc2fd2836d',
}
UNITS = ['pipewire.socket', 'pipewire-pulse.socket', 'wireplumber.service',
         'pipewire-pulse.service', 'pipewire.service']

def require(ok, message):
    if not ok:
        raise ValueError(message)

def run(args, **kw):
    return subprocess.run(list(map(str, args)), check=True, **kw)

def output(args):
    return run(args, capture_output=True, text=True).stdout.strip()

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def packages(root=Path('/')):
    result = {}
    for block in (root/'lib/apk/db/installed').read_text().split('\n\n'):
        fields = dict(line.split(':', 1) for line in block.splitlines() if line.startswith(('P:', 'V:')))
        if 'P' in fields:
            require('V' in fields and fields['P'] not in result, 'Invalid installed APK database')
            result[fields['P']] = fields['V']
    require(bool(result), 'Empty installed APK database')
    return result

def changed(before, after):
    return {n for n in before.keys() | after.keys() if before.get(n) != after.get(n)}

def compatibility_gate(checker_env=None):
    command = ['python3', KIT/'packaging/postmarketos/duet-camera-check.py',
               '--abi', KIT/'packaging/postmarketos/kernel-abi.json']
    result = json.loads(output(command) if checker_env is None else
                        run(command, capture_output=True, text=True, env=checker_env).stdout)
    require(result['compatible'], 'The running kernel, camera DT and firmware must match this update')
    require('apk-tools 3.' in output(['apk', '--version']), 'apk-tools 3 is required')

def usb_gate(checker_env=None):
    require(os.geteuid() == 0, 'Run with sudo on the external test OS')
    mounts = {}
    for mount in ('/', '/boot'):
        dev = output(['findmnt', '-n', '-o', 'SOURCE', mount])
        require(dev.startswith('/dev/'), 'A direct USB root and boot mount are required')
        p = (Path('/sys/class/block')/Path(dev).resolve().name).resolve()
        require('/usb' in str(p) and (p/'partition').exists(), 'Root/boot must be USB partitions')
        mounts[mount] = (str(p.parent), output(['blkid', '-s', 'UUID', '-o', 'value', dev]))
    require(mounts['/'][0] == mounts['/boot'][0], 'Root and boot must share one USB')
    found = False
    for p in Path('/sys/block').glob('mmcblk[0-9]*'):
        kind = p/'device/type'
        if kind.exists() and kind.read_text().strip() == 'MMC':
            found = True
            for q in [p, *p.glob(p.name+'p*'), *p.parent.glob(p.name+'boot*')]:
                require((q/'ro').read_text().strip() == '1', 'Internal eMMC protection is missing')
    require(found, 'Protected eMMC was not found')
    compatibility_gate(checker_env)
    return {m: x[1] for m, x in mounts.items()}

def internal_gate(checker_env=None):
    """Check the explicitly selected internal OS without changing protections."""
    require(os.geteuid() == 0, 'Run with sudo on the internal Duet OS')
    mounts = {}
    for mount in ('/', '/boot'):
        source = output(['findmnt', '-n', '-o', 'SOURCE', '--mountpoint', mount])
        require(source.startswith('/dev/') and len(source.splitlines()) == 1,
                'Internal mode requires direct root and boot block mounts')
        device = Path(source).resolve(strict=True)
        require(re.fullmatch(r'mmcblk[0-9]+p[0-9]+', device.name),
                'Internal mode requires eMMC partitions')
        node = (Path('/sys/class/block')/device.name).resolve(strict=True)
        require((node/'partition').is_file() and re.fullmatch(r'mmcblk[0-9]+', node.parent.name)
                and not any(re.fullmatch(r'usb[0-9]+', part) for part in node.parts),
                'Internal mode requires physical eMMC partitions')
        require((node.parent/'device/type').read_text().strip() == 'MMC',
                'Internal mode does not accept an SD card')
        options = output(['findmnt', '-n', '-o', 'OPTIONS', '--mountpoint', mount]).split(',')
        require(('rw' if mount == '/' else 'ro') in options,
                'Internal root must be writable and /boot must be mounted read-only')
        uuid = output(['blkid', '-s', 'UUID', '-o', 'value', device])
        require(uuid and len(uuid.splitlines()) == 1, 'A unique filesystem UUID is required')
        mounts[mount] = (node, uuid)
    disk = mounts['/'][0].parent
    require(disk == mounts['/boot'][0].parent and mounts['/'][0] != mounts['/boot'][0],
            'Internal root and boot must be separate partitions of the same eMMC')
    require(mounts['/'][1] != mounts['/boot'][1], 'Root and boot filesystem UUIDs must differ')
    cid = (disk/'device/cid').read_text().strip().lower()
    require(re.fullmatch(r'[0-9a-f]{32}', cid), 'A valid eMMC CID is required')
    for suffix in ('boot0', 'boot1'):
        boot = Path('/sys/class/block')/(disk.name+suffix)
        require((boot/'ro').read_text().strip() == '1', 'eMMC hardware boot protection is missing')
    compatibility_gate(checker_env)
    return {**{m: x[1] for m, x in mounts.items()}, 'emmc_cid': cid}

def backup_matches(record, identity, mode):
    # Backups made before modes were introduced always belong to the USB path.
    return record.get('mode', 'external-usb') == mode and record['identity'] == identity

def key_fingerprint(value):
    if not re.fullmatch(r'[0-9a-fA-F]{64}', value):
        raise argparse.ArgumentTypeError('Expected a 64-digit SHA256 public-key fingerprint')
    return value.lower()

def public_key_der(pem):
    """Decode one canonical PUBLIC KEY PEM block (SPKI), without a CLI dependency."""
    match = re.fullmatch(rb'-----BEGIN PUBLIC KEY-----\r?\n((?:[A-Za-z0-9+/=]+\r?\n)+)'
                         rb'-----END PUBLIC KEY-----(?:\r?\n)?', pem)
    require(match is not None, 'Expected exactly one SPKI PUBLIC KEY PEM block')
    encoded = b''.join(match[1].splitlines())
    try:
        der = base64.b64decode(encoded, validate=True)
    except ValueError as error:
        raise ValueError('Invalid public-key base64') from error
    require(der and base64.b64encode(der) == encoded, 'Non-canonical public-key base64')
    return der

def selected_apks(directory, expected_key_sha256=KEY_HASH):
    manifest = json.loads((directory/'packages.json').read_text())
    keys = list((directory/'keys').glob('*.pub'))
    require(len(keys) == 1, 'Expected one signing public key')
    der = public_key_der(keys[0].read_bytes())
    require(hashlib.sha256(der).hexdigest() == expected_key_sha256, 'Signing public key mismatch')
    selected = []
    for name, version in PINS.items():
        filename = name+'-'+version+'.apk'
        p = directory/'repo/aarch64'/filename
        info = manifest[filename]
        require(p.is_file() and not p.is_symlink(), 'Missing regular package: '+filename)
        require(info['name'] == name and info['version'] == version and info['arch'] == 'aarch64', 'Wrong package identity')
        require(sha(p) == info['sha256'], 'Package digest mismatch: '+filename)
        selected.append(p)
    run(['apk', 'verify', '--keys-dir', directory/'keys', *selected])
    return selected, keys[0]

def official_zstd_inputs(directory):
    """Return only the fixed official supplement; do not expand package trust."""
    require(isinstance(OFFICIAL_ZSTD['apk_sha256'], str) and
            re.fullmatch(r'[0-9a-f]{64}', OFFICIAL_ZSTD['apk_sha256']),
            'The official zstd supplement has not been pinned; installation is unavailable')
    supplement = directory/'official-zstd'
    require(not supplement.is_symlink(), 'Official supplement directory must not be a symlink')
    apk = supplement/('zstd-'+OFFICIAL_ZSTD['version']+'.apk')
    index = supplement/'APKINDEX.tar.gz'
    for path, expected in ((apk, OFFICIAL_ZSTD['apk_sha256']), (index, OFFICIAL_ZSTD['index_sha256'])):
        require(path.is_file() and not path.is_symlink(), 'Missing regular official supplement: '+path.name)
        require(sha(path) == expected, 'Official supplement digest mismatch: '+path.name)
    return apk, index

def selected_zstd(directory, before, trusted_keys=Path('/etc/apk/keys')):
    # Never replace a previously installed zstd or broaden the dependency set.
    if 'zstd' in before:
        return []
    apk, index = official_zstd_inputs(directory)
    key = trusted_keys/OFFICIAL_ZSTD['key_name']
    require(key.is_file() and not key.is_symlink() and sha(key) == OFFICIAL_ZSTD['key_sha256'],
            'The pinned official zstd key must already be trusted by the OS')
    with tempfile.TemporaryDirectory(prefix='duet-official-zstd-') as tmp:
        keys = Path(tmp)/'keys'; keys.mkdir(); shutil.copy2(key, keys/key.name)
        # Independent trust boundary: this directory has the official key only,
        # never the camera author's key. APK and signed-index validation must
        # both succeed before the APK joins the normal camera transaction.
        run(['apk', 'verify', '--keys-dir', keys, apk])
        planned, _ = sandbox(['zstd='+OFFICIAL_ZSTD['version']], keys, repositories=[index.parent])
        require(changed(before, planned) == {'zstd'} and planned.get('zstd') == OFFICIAL_ZSTD['version'],
                'Official zstd would change other packages; refusing the supplement')
    return [apk]

@contextlib.contextmanager
def checker_zstd(directory, trusted_keys=Path('/etc/apk/keys')):
    """Use a verified temporary decoder only for read-only firmware checking.

    No package is installed here. The full apply and reverse transaction must
    still pass before the official zstd APK can be added to the running OS.
    """
    require(os.geteuid() == 0, 'Run with sudo on the selected Duet OS')
    if shutil.which('zstd'):
        yield None
        return
    require('apk-tools 3.' in output(['apk', '--version']), 'apk-tools 3 is required')
    apks = selected_zstd(directory, packages(), trusted_keys)
    require(len(apks) == 1, 'Installed zstd has no executable; repair it before applying or restoring')
    with tempfile.TemporaryDirectory(prefix='duet-checker-zstd-') as tmp:
        temporary = Path(tmp)
        keys = temporary/'keys'; keys.mkdir()
        key = trusted_keys/OFFICIAL_ZSTD['key_name']; shutil.copy2(key, keys/key.name)
        extracted = temporary/'root'; extracted.mkdir()
        run(['apk', 'extract', '--keys-dir', keys, '--no-chown', '--destination', extracted, apks[0]])
        decoder = extracted/'usr/bin/zstd'
        require(decoder.is_file() and not decoder.is_symlink(), 'Official zstd executable is missing')
        version = output([decoder, '--version'])
        require(re.search(r'\bv1\.5\.7\b', version), 'Unexpected official zstd CLI version')
        environment = dict(os.environ)
        environment['PATH'] = str(decoder.parent)+os.pathsep+environment.get('PATH', '/usr/sbin:/usr/bin:/sbin:/bin')
        yield environment

def apk_add(root, apks, keys, simulate=False, repositories=()):
    # No network or package scripts: the fixed set contains no package scripts.
    # depmod and service setup are performed explicitly after the transaction.
    args = ['apk', '--root', root, '--keys-dir', keys, '--repositories-file', '/dev/null',
            '--no-network', '--scripts=no', '--commit-hooks=no']
    for repo in repositories:
        args += ['--repository', Path(repo)/'APKINDEX.tar.gz']
    args.append('add')
    if simulate:
        args.append('--simulate')
    try:
        return run([*args, *apks], capture_output=True, text=True).stdout
    except subprocess.CalledProcessError as error:
        raise RuntimeError('APK transaction failed:\n'+error.stdout+error.stderr) from error

def sandbox(apks, keys, world=None, source_root=Path('/'), repositories=(), first_apks=(), config_paths=None):
    with tempfile.TemporaryDirectory(prefix='duet-apk-plan-') as tmp:
        root = Path(tmp).resolve()
        shutil.copytree(source_root/'lib/apk/db', root/'lib/apk/db')
        (root/'etc/apk').mkdir(parents=True)
        original_world = world if world is not None else (source_root/'etc/apk/world').read_bytes()
        (root/'etc/apk/world').write_bytes((source_root/'etc/apk/world').read_bytes())
        if config_paths is not None:
            # APK protects edited configuration with .apk-new files. Seed the
            # same objects so its predicted config result matches the real OS.
            configs = snapshot(config_paths, source_root)
            write_configs(configs, root)
        if first_apks:
            apk_add(root, first_apks, keys)
        (root/'etc/apk/world').write_bytes(original_world)
        log = apk_add(root, apks, keys, repositories=repositories)
        result = packages(root), log
        return (*result, snapshot(config_paths, root)) if config_paths is not None else result

def preserve_packages(specs, destination, cache=Path('/var/cache/apk')):
    """Prefer the installed OS cache; validate its APKs against signed indexes later."""
    missing = []
    for spec in specs:
        name, version = spec.split('=', 1)
        filename = name+'-'+version+'.apk'
        candidates = [cache/filename, *sorted(cache.glob(name+'-'+version+'.*.apk'))]
        candidates = [p for p in candidates if p.is_file() and not p.is_symlink()]
        if not candidates:
            missing.append(spec)
            continue
        require(len({sha(p) for p in candidates}) == 1, 'Conflicting cached original APKs: '+filename)
        shutil.copy2(candidates[0], destination/filename)
    if missing:
        # apk 3's non-recursive fetch treats inputs as name queries rather
        # than version constraints. Resolve constraints without downloading
        # dependencies, then retrieve only the exact originals we need.
        urls = output(['apk', 'fetch', '--recursive', '--simulate', '--url', *missing]).splitlines()
        for spec in missing:
            name, version = spec.split('=', 1)
            filename = name+'-'+version+'.apk'
            matches = sorted(set(u for u in urls if urlparse(u).scheme in ('https', 'http')
                                 and unquote(urlparse(u).path).split('/')[-1] == filename))
            require(len(matches) == 1, 'Original APK URL is missing or ambiguous: '+filename)
            with urlopen(matches[0], timeout=30) as source, (destination/filename).open('xb') as target:
                shutil.copyfileobj(source, target)

def preserve_repositories(directory, cache=Path('/var/cache/apk')):
    """Keep the original signed indexes: some pmOS APKs trust the index only."""
    seen = set(); repositories = []
    for index in sorted(cache.glob('APKINDEX.*.tar.gz')):
        digest = sha(index)
        if digest in seen: continue
        seen.add(digest)
        repo = directory/'repositories'/digest; repo.mkdir(parents=True)
        shutil.copy2(index, repo/'APKINDEX.tar.gz')
        for apk in (directory/'apks').glob('*.apk'):
            os.link(apk, repo/apk.name)
        repositories.append(repo)
    require(repositories, 'Original signed repository indexes are missing')
    return repositories

def recoverable_packages(actual, before, after, status):
    if status == 'APPLIED':
        return actual == after
    # Interrupted transactions may contain only old/new versions within the
    # planned delta. Unrelated installs, removals or upgrades are refused.
    delta = changed(before, after)
    return all(actual.get(n) == before.get(n) for n in (before.keys() | after.keys() | actual.keys()) - delta) and all(
        actual.get(n) in (before.get(n), after.get(n)) for n in delta)

def stopped_capture(text):
    values = dict(x.split('=', 1) for x in text.split() if '=' in x)
    common = all(values.get(n) == '0' for n in ('capture_error', 'input_error', 'pending', 'refs'))
    never_started = 'unpublished_idle' not in values and all(values.get(n) == '0' for n in ('epoch', 'sof_count', 'done_count'))
    unpublished = values.get('unpublished_idle') == '1' and values.get('inputs_idle') == '1' and all(values.get(n) == '0' for n in ('sof_count', 'done_count'))
    finalized = values.get('inputs_idle') == '1' and values.get('finalized') == '1'
    return common and (never_started or unpublished or finalized)

def runtime_files(internal_emmc=False, username=None):
    here = KIT/'packaging/postmarketos'
    spec = importlib.util.spec_from_file_location('scp_init', here/'external-scp-init.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    mode_option = ' --internal-emmc' if internal_emmc else ''
    activation_option = '--internal-emmc' if internal_emmc else '--external-usb'
    files = {
        'etc/modprobe.d/duet-camera-scp-isolation.conf': (module.INTERNAL_CONFIG if internal_emmc else module.CONFIG).encode(),
        'etc/modprobe.d/duet-camera-first-install.conf': b'blacklist ov02a10\nblacklist ov8856\nblacklist dw9768\nblacklist mtk_seninf\nblacklist mt8183_p1\n',
        'usr/libexec/duet-camera-external-scp-init': (here/'external-scp-init.py').read_bytes(),
        'usr/lib/systemd/system/duet-camera-scp-init.service': ('[Unit]\nDescription=Initialize isolated Duet SCP once\nAfter=systemd-udev-trigger.service\nBefore=display-manager.service duet-camera-activate.service\n\n[Service]\nType=oneshot\nExecStart=/usr/libexec/duet-camera-external-scp-init'+mode_option+'\nTimeoutStartSec=20\nRemainAfterExit=yes\n\n[Install]\nWantedBy=multi-user.target\n').encode(),
        'usr/lib/systemd/system/duet-camera-activate.service': ('[Unit]\nDescription=Activate matched Duet camera modules\nConditionPathExists=/usr/bin/duet-camera-activate\nRequires=duet-camera-scp-init.service\nAfter=duet-camera-scp-init.service\nBefore=display-manager.service\n\n[Service]\nType=oneshot\nExecStart=/usr/bin/duet-camera-activate '+activation_option+'\nRemainAfterExit=yes\n\n[Install]\nWantedBy=multi-user.target\n').encode(),
    }
    for name in ('duet-camera-check.py', 'kernel-abi.json', 'camera-nodes.json', 'fdt.py', 'integrate-camera-dtb.py', 'legacy_bindings.py'):
        files['usr/libexec/duet-camera-external-check/'+name] = (here/name).read_bytes()
    for unit in ('duet-camera-scp-init.service', 'duet-camera-activate.service'):
        files['etc/systemd/system/multi-user.target.wants/'+unit] = ('link', '/usr/lib/systemd/system/'+unit)
    if username is not None:
        require(pwd.getpwnam(username).pw_uid != 0 and re.fullmatch(r'[A-Za-z0-9_.-]+', username), 'Invalid desktop user')
        unit='duet-camera-user-media.service'
        files['usr/lib/systemd/user/'+unit]=('[Unit]\nDescription=Start Duet camera media session\nConditionUser='+username+'\n\n[Service]\nType=oneshot\nExecStart=/usr/bin/systemctl --user start pipewire.service wireplumber.service\nRemainAfterExit=yes\n\n[Install]\nWantedBy=default.target\n').encode()
        files['etc/systemd/user/default.target.wants/'+unit]=('link','/usr/lib/systemd/user/'+unit)
    return files

def snapshot(paths, root=None):
    root = Path('/') if root is None else root
    result = {}
    for name in paths:
        require(not Path(name).is_absolute() and '..' not in Path(name).parts, 'Invalid backup path')
        p = root/name
        for parent in p.parents:
            require(not parent.is_symlink(), 'Symlink parent for config: '+name)
        if p.is_symlink():
            st = p.lstat()
            result[name] = {'link': os.readlink(p), 'mode': stat.S_IMODE(st.st_mode), 'uid': st.st_uid, 'gid': st.st_gid}
        elif p.exists():
            require(p.is_file(), 'Expected a regular config file: '+name)
            st = p.stat()
            result[name] = {'data': base64.b64encode(p.read_bytes()).decode(), 'mode': stat.S_IMODE(st.st_mode), 'uid': st.st_uid, 'gid': st.st_gid}
        else:
            result[name] = None
    return result

def save_state(record, directory):
    p = directory/'state.json'
    tmp = p.with_suffix('.tmp')
    with tmp.open('w') as f:
        os.fchmod(f.fileno(), 0o600)
        f.write(json.dumps(record, indent=2)+'\n'); f.flush(); os.fsync(f.fileno())
    tmp.replace(p)
    fd = os.open(directory, os.O_RDONLY)
    try: os.fsync(fd)
    finally: os.close(fd)

def config_guard(record):
    """Check every config before any package/world/config restoration write."""
    before = record['configs']
    require(record['status'] in ('APPLIED', 'APPLYING', 'RESTORING'), 'No configuration restore from this state')
    require('config_states' in record,
            'Backup lacks expected configuration states; keep it for reviewed recovery. Automatic restore is unsafe.')
    states = record['config_states']
    require(set(states) >= {'apk', 'after'} and all(set(s) == set(before) for s in states.values()),
            'Incomplete expected configuration states')
    actual = snapshot(before)
    allowed = [states['after']] if record['status'] == 'APPLIED' else [before, *states.values()]
    conflicts = [name for name in before if not any(actual[name] == s[name] for s in allowed)]
    require(not conflicts, 'Configurations changed outside this update; refuse overwrite: '+', '.join(sorted(conflicts)))
    return actual

def planned_runtime(configs, files, key):
    return {**configs, **runtime_snapshots({**files, 'etc/apk/keys/'+key.name: key.read_bytes()})}

def runtime_snapshots(files):
    result = {}
    for name, data in files.items():
        meta = {'mode': 0o777 if isinstance(data, tuple) else (0o755 if name.endswith('duet-camera-external-scp-init') else 0o644), 'uid': 0, 'gid': 0}
        result[name] = dict(meta, **({'link': data[1]} if isinstance(data, tuple) else {'data': base64.b64encode(data).decode()}))
    return result

def stop_session(username):
    user = pwd.getpwnam(username)
    require(user.pw_uid != 0, 'Select the desktop user, not root')
    for p in Path('/proc').iterdir():
        if not p.name.isdecimal():
            continue
        try:
            comm = (p/'comm').read_text().strip()
            require(comm not in ('snapshot', 'cam', 'qcam', 'gst-launch-1.0'), 'Close the camera application first')
            if comm in ('pipewire', 'wireplumber', 'pipewire-pulse'):
                require(p.stat().st_uid == user.pw_uid, 'Another user has an active media session')
        except FileNotFoundError:
            pass
    diagnostics = list(Path('/sys/kernel/debug/mt8183_p1').glob('*/capture_result'))
    for p in diagnostics:
        require(stopped_capture(p.read_text()), 'Camera did not stop cleanly')
    if Path('/sys/module/mt8183_p1').exists():
        require(len(diagnostics) == 1, 'Loaded camera driver has no unique stop diagnostic')
        require(Path('/sys/class/remoteproc/remoteproc0/state').read_text().strip() == 'offline', 'Camera SCP is busy')
    command = ['runuser', '-u', username, '--', 'env', f'XDG_RUNTIME_DIR=/run/user/{user.pw_uid}',
               f'DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/{user.pw_uid}/bus', 'systemctl', '--user']
    run([*command, 'mask', '--runtime', '--now', *UNITS])

def install_runtime(files):
    write_configs(runtime_snapshots(files))

def write_configs(configs, root=None):
    """Replace each object atomically, so interruption leaves a known state."""
    root = Path('/') if root is None else root
    snapshot(configs, root)
    for name, item in configs.items():
        p = root/name
        if item is None:
            if p.exists() or p.is_symlink(): p.unlink()
        else:
            p.parent.mkdir(parents=True, exist_ok=True)
            fd, temporary = tempfile.mkstemp(prefix='.duet-config-', dir=p.parent)
            try:
                if 'link' in item:
                    os.close(fd); fd = -1; os.unlink(temporary)
                    os.symlink(item['link'], temporary); os.lchown(temporary, item['uid'], item['gid'])
                else:
                    os.fchown(fd, item['uid'], item['gid']); os.fchmod(fd, item['mode'])
                    with os.fdopen(fd, 'wb', closefd=False) as f:
                        f.write(base64.b64decode(item['data'])); f.flush(); os.fsync(fd)
                    os.close(fd); fd = -1
                os.replace(temporary, p)
            finally:
                if fd >= 0: os.close(fd)
                if os.path.lexists(temporary): os.unlink(temporary)
        if p.parent.exists():
            fd = os.open(p.parent, os.O_RDONLY)
            try: os.fsync(fd)
            finally: os.close(fd)

def restore_configs(record):
    config_guard(record)
    write_configs(record['configs'])
    require(snapshot(record['configs']) == record['configs'], 'Restored configurations differ from backup')

def finish_update(action, record, directory):
    run(['depmod', '-a', output(['uname', '-r'])])
    run(['systemctl', 'daemon-reload']); run(['sync'])
    record['status'] = 'RESTORED' if action == 'restore' else 'APPLIED'
    save_state(record, directory)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('action', choices=('check', 'apply', 'restore'))
    ap.add_argument('--packages', type=Path, default=KIT.parent/'packages')
    ap.add_argument('--expected-key-sha256', type=key_fingerprint, default=KEY_HASH,
                    help='Independently verified signing-key fingerprint for a developer build')
    ap.add_argument('--user', default=os.environ.get('SUDO_USER'))
    ap.add_argument('--internal-emmc', action='store_true',
                    help='Explicitly update the internal eMMC OS; requires read-only /boot and matched hardware')
    a = ap.parse_args()
    mode = 'internal-emmc' if a.internal_emmc else 'external-usb'
    with checker_zstd(a.packages.resolve()) as checker_env:
        identity = internal_gate(checker_env) if a.internal_emmc else usb_gate(checker_env)
    STATE.mkdir(mode=0o700, parents=True, exist_ok=True)
    require(not STATE.is_symlink() and STATE.stat().st_uid == 0, 'Unsafe state directory')
    STATE.chmod(0o700)
    with (STATE/'lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        latest = STATE/'latest'
        current = json.loads((latest.resolve()/'state.json').read_text()) if latest.exists() else None
        if a.action == 'restore':
            require(current and current['status'] in ('APPLIED', 'APPLYING', 'RESTORING'), 'No update to restore')
            directory = latest.resolve()
            require(backup_matches(current, identity, mode), 'This backup belongs to another storage mode or root/boot')
            require(recoverable_packages(packages(), current['before'], current['after'], current['status']), 'Packages changed outside this update; use the saved OS recovery backup')
            admitted_configs = config_guard(current)
            old = [directory/'apks'/name for name in current['old_apks']]
            for p in old:
                require(sha(p) == current['old_apks'][p.name], 'Backup APK digest mismatch')
            repositories = [directory/'repositories'/name for name in current['repository_indexes']]
            for repo in repositories:
                require(sha(repo/'APKINDEX.tar.gz') == repo.name, 'Backup index digest mismatch')
            world = base64.b64decode(current['world'])
            planned, log, restored_apk_configs = sandbox(current['old_specs'], directory/'keys', world,
                repositories=repositories, config_paths=current['configs'])
            require(planned == current['before'], 'Offline restore cannot reproduce the original packages')
            require(a.user, 'Use sudo from the desktop session, or specify --user')
            stop_session(a.user)
            require(config_guard(current) == admitted_configs, 'Configurations changed while planning restoration')
            current['config_states']['restore_before'] = admitted_configs
            current['config_states']['restore_apk'] = restored_apk_configs
            current['status'] = 'RESTORING'; save_state(current, directory)
            write_configs({'etc/apk/world': current['configs']['etc/apk/world']})
            print(apk_add('/', current['old_specs'], directory/'keys', repositories=repositories), flush=True)
            require(packages() == current['before'], 'Restored package set differs from backup')
            write_configs({'etc/apk/world': current['configs']['etc/apk/world']})
            restore_configs(current)
            record = current
        else:
            apks, key = selected_apks(a.packages.resolve(), a.expected_key_sha256)
            with tempfile.TemporaryDirectory(prefix='duet-trusted-') as tmp:
                keys = Path(tmp)/'keys'; shutil.copytree('/etc/apk/keys', keys); shutil.copy2(key, keys/key.name)
                before = packages()
                supplemental = selected_zstd(a.packages.resolve(), before)
                apks = [*apks, *supplemental]
                files = runtime_files(a.internal_emmc, a.user) if a.action == 'apply' else runtime_files(a.internal_emmc)
                config_paths = set(files) | {'etc/apk/world', 'etc/modprobe.d/duet-camera.conf', 'etc/modprobe.d/duet-camera.conf.apk-new',
                    'etc/apk/keys/'+key.name, 'etc/systemd/system/duet-camera-activate.service.d/10-scp-init.conf'}
                before_configs = snapshot(config_paths)
                require(before_configs['etc/apk/world'] is not None and 'data' in before_configs['etc/apk/world'],
                        'apk world must be a regular file')
                after, log, apk_configs = sandbox(apks, keys, config_paths=config_paths)
                delta = changed(before, after)
                allowed_removed = {'gtest', 'libpisp', 'roc-toolkit-libs', 'libunwind', 'libuv', 'webrtc-audio-processing-2'}
                allowed_added = {'zstd'} if supplemental else set()
                require(delta <= PINS.keys() | allowed_removed | allowed_added, 'Unexpected package changes: '+str(sorted(delta)))
                require(all(n not in after for n in delta & allowed_removed), 'An unrelated dependency would be changed')
                require(not supplemental or ('zstd' not in before and after.get('zstd') == OFFICIAL_ZSTD['version']),
                        'The official supplement may only add the pinned zstd version')
                require(all(after.get(n) == v for n, v in PINS.items()), 'Update plan did not select all pinned packages')
                print(log, flush=True)
                if a.action == 'check':
                    print('CHECK PASS: matched '+mode+'/kernel/DT/FW, signed packages and restricted package plan. No update applied.'); return
                require(not current or current['status'] == 'RESTORED', 'Restore or resolve the previous update first')
                require(a.user, 'Use sudo from the desktop session, or specify --user')
                directory = STATE/datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
                directory.mkdir(mode=0o700); shutil.copytree(keys, directory/'keys'); (directory/'apks').mkdir()
                old_specs = [n+'='+before[n] for n in sorted(delta) if n in before]
                require(bool(old_specs), 'No original packages selected for backup')
                preserve_packages(old_specs, directory/'apks')
                old = sorted((directory/'apks').glob('*.apk'))
                require(len(old) == len(old_specs), 'Original package backup is incomplete')
                repositories = preserve_repositories(directory)
                # A temporary installation verifies the signed-index-to-APK
                # chain, including pmOS APKs without standalone signatures.
                old_plan, _ = sandbox(old_specs, directory/'keys', repositories=repositories, first_apks=apks)
                require(old_plan == before, 'Original packages cannot be restored from the signed local repositories')
                record = {'status': 'READY', 'mode': mode, 'identity': identity, 'before': before, 'after': after,
                          'world': before_configs['etc/apk/world']['data'],
                          'configs': before_configs, 'config_states': {'apk': apk_configs,
                              'after': planned_runtime(apk_configs, files, key)}, 'old_apks': {p.name: sha(p) for p in old},
                          'old_specs': old_specs, 'repository_indexes': [p.name for p in repositories]}
                save_state(record, directory)
                require(packages() == before, 'Package set changed while preparing backup')
                stop_session(a.user)
                require(snapshot(config_paths) == before_configs, 'Configurations changed while preparing backup')
                if latest.is_symlink(): latest.unlink()
                latest.symlink_to(directory.name)
                record['status'] = 'APPLYING'; save_state(record, directory)
                print(apk_add('/', apks, directory/'keys'), flush=True)
                require(packages() == after, 'Installed package set differs from the checked plan')
                require(snapshot(config_paths) == apk_configs, 'Installed package configurations differ from the checked plan')
                install_runtime({'etc/apk/keys/'+key.name: key.read_bytes()})
                install_runtime(files)
                require(snapshot(config_paths) == record['config_states']['after'], 'Installed configurations differ from the checked plan')
                run(['/usr/bin/duet-camera-check'])
        finish_update(a.action, record, directory)
        next_step = 'Restart to use the camera.' if a.action == 'apply' else 'Restart to use the restored environment.'
        print(a.action.upper()+' COMPLETE. '+next_step+' Backup: '+str(directory), flush=True)

if __name__ == '__main__':
    main()
