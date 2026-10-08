#!/usr/bin/env python3
# Modified on 2026-10-05: publication review fixes; see docs/PUBLICATION_REVIEW.json.
# SPDX-License-Identifier: GPL-2.0-only
"""Export files for a boot integrator; never modifies /boot or a block device."""
from pathlib import Path
import argparse,hashlib,json,importlib


def require(condition, message="Invalid or incompatible input"):
    if not condition:
        raise ValueError(message)

ap=argparse.ArgumentParser()
ap.add_argument('--base-dtb',required=True);ap.add_argument('--output',required=True)
ap.add_argument('--image',required=True,help='existing matched uncompressed kernel Image')
ap.add_argument('--abi',default=str(Path(__file__).with_name('kernel-abi.json')))
args=ap.parse_args();src=Path(args.base_dtb);image=Path(args.image);out=Path(args.output)
abi=json.loads(Path(args.abi).read_text());digest=lambda b:hashlib.sha256(b).hexdigest()
require(image.is_file() and not image.is_symlink(), 'kernel asset mismatch')
require(not out.exists() and not out.is_symlink(), 'output must be a new directory')
spec=json.loads(Path(__file__).with_name('camera-nodes.json').read_text())
image_data=image.read_bytes()
require(digest(image_data)==abi['image_sha256'], 'kernel asset mismatch')
base_data=src.read_bytes()
data,audit=importlib.import_module('integrate-camera-dtb').integrate(base_data,spec)
out.mkdir(parents=True);(out/'Image').write_bytes(image_data);(out/'mt8183-kukui-krane-sku176.dtb').write_bytes(data)
record=dict(**audit,kernel_image_sha256=abi['image_sha256'],base_dtb_sha256=digest(base_data),camera_dtb_sha256=digest(data),boot_or_media_written=False,firmware_included=False)
(out/'manifest.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record),flush=True)
