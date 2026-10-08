#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify this exact v2 kernel APK through a trusted signed repository index."""
import argparse, base64, hashlib, io, json, subprocess, tarfile, tempfile, zlib
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('apk', type=Path)
p.add_argument('index', type=Path)
p.add_argument('trusted_repository_key', type=Path)
a = p.parse_args()
expected = '7729c6db0bff84ace07dd55e6f67afc8c3f82982989528d0a055631afb5afd4d'
apk = a.apk.read_bytes()
if hashlib.sha256(apk).hexdigest() != expected:
    raise SystemExit('Wrong kernel APK SHA256')

def split(data):
    d = zlib.decompressobj(31)
    clear = d.decompress(data)
    if not d.eof:
        raise SystemExit('Truncated gzip stream')
    return data[:len(data)-len(d.unused_data)], clear, d.unused_data

_, signature_tar, signed_index = split(a.index.read_bytes())
with tarfile.open(fileobj=io.BytesIO(signature_tar)) as t:
    signature = t.extractfile('.SIGN.RSA.build.postmarketos.org.rsa.pub').read()
with tempfile.TemporaryDirectory() as folder:
    folder = Path(folder)
    (folder/'signature').write_bytes(signature)
    (folder/'index').write_bytes(signed_index)
    subprocess.run(['openssl', 'dgst', '-sha1', '-verify',
                    str(a.trusted_repository_key), '-signature',
                    str(folder/'signature'), str(folder/'index')], check=True)
_, index_tar, remainder = split(signed_index)
if remainder:
    raise SystemExit('Unexpected trailing index stream')
with tarfile.open(fileobj=io.BytesIO(index_tar)) as t:
    index = t.extractfile('APKINDEX').read().decode()
entries = [dict(line.split(':', 1) for line in block.splitlines() if ':' in line)
           for block in index.split('\n\n')]
entries = [e for e in entries if e.get('P') == 'linux-postmarketos-mediatek-mt81'
           and e.get('V') == '6.18.28-r0' and e.get('A') == 'aarch64']
if len(entries) != 1 or int(entries[0]['S']) != len(apk):
    raise SystemExit('Exact kernel APK absent from signed index')
_, _, unsigned_apk = split(apk)
control, control_tar, data = split(unsigned_apk)
checksum = 'Q1'+base64.b64encode(hashlib.sha1(control).digest()).decode()
if checksum != entries[0]['C']:
    raise SystemExit('Kernel control checksum disagrees with signed index')
with tarfile.open(fileobj=io.BytesIO(control_tar)) as t:
    info = dict(line.split(' = ', 1) for line in
                t.extractfile('.PKGINFO').read().decode().splitlines()
                if ' = ' in line)
if hashlib.sha256(data).hexdigest() != info['datahash']:
    raise SystemExit('Kernel data checksum disagrees with verified control')
print(json.dumps({'official_index_signature_verified': True,
                  'apk_control_and_data_verified': True,
                  'apk_sha256': expected}))
