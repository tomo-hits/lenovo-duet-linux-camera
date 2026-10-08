#!/bin/sh
# Modified on 2026-10-05: publication review fixes; see docs/PUBLICATION_REVIEW.json.
# SPDX-License-Identifier: GPL-2.0-only
# Build from a fresh Linux tree; no target deployment or kernel Image build.
set -eu
[ "$#" = 7 ] || { echo 'usage: build-kernel.sh OUTPUT LINUX_TAR IMAGE SYSTEM_MAP PUBLIC_CONFIG MODULE_ROOT JOBS' >&2; exit 2; }
out=$1; archive=$2; image=$3; map=$4; config=$5; providers=$6; jobs=$7
kit=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
[ "$(uname -m)" = aarch64 ]
test -x /sbin/modinfo
/sbin/modinfo --version | grep -F kmod
clang --version | head -1 | grep -F 'Alpine clang version 22.1.3'
[ ! -e "$out" ]; mkdir -p "$out"
out=$(CDPATH= cd -- "$out" && pwd)
printf '%s  %s\n' '5acc1a97ae0bca7aeb7fcac04fd3b7bafea5ff976dfe5d42c7015e084d9ee3104c3945880846dfb185a1010d0bad8c7a229b23febdef44306d19fb494a03b1a6' "$archive" | sha512sum -c -
printf '%s  %s\n' 'cce4914459d15558b54640f360e0a58476edfa74595fe30143d29dce7049d5a1' "$image" '3757e93b5d3cfc162350534b637810fbc0807fed47b644cfc888bd372c664329' "$map" '780e607deea636e4ffba18ac49be4b36926d0c2e1308d380d6cb764c604d6756' "$config" | sha256sum -c -
tar -C "$out" -xf "$archive"
kernel=$out/linux-6.18.28
for name in mt8183-fix-bluetooth.patch mt8186-enable-dpi.patch mt8186-ASoC-hdmi-codec-Add-event-handler-for-hdmi-TX.patch mt8186-SoC-mediatek-mt8186-correct-the-HDMI-widgets.patch mt8186-drm-bridge-it6505-Add-audio-support.patch mt8186-ASoC-mediatek-mt8186-make-FE-nonatomic-and-no_pcm.patch mtk-pmdomain.patch; do
 patch -d "$kernel" -p1 < "$kit/configs/kernel/$name"
done
cp "$kit/configs/kernel/matched.config" "$kernel/.config"
make -C "$kernel" ARCH=arm64 LLVM=1 olddefconfig modules_prepare -j"$jobs" > "$out/prepare.log" 2>&1
cmp "$kit/configs/kernel/matched.config" "$kernel/.config"
python3 - "$kernel" "$kit/configs/kernel/selected-source-sha256.json" <<'PYVERIFY'
import hashlib,json,sys
from pathlib import Path
root=Path(sys.argv[1]); expected=json.loads(Path(sys.argv[2]).read_text())
if not all(hashlib.sha256((root/n).read_bytes()).hexdigest()==h for n,h in expected.items()):
    raise SystemExit('Source/provider hash mismatch')
print('Matched SDK selected source files:',len(expected))
PYVERIFY
# The SDK has no Module.symvers. Resolve actual Image/media/SCP exports instead.
[ ! -e "$kernel/Module.symvers" ]
python3 - "$providers" "$kit/configs/kernel/public-provider-sha256.json" <<'PYCHECK'
import hashlib,json,sys
from pathlib import Path
root=Path(sys.argv[1]);expected=json.loads(Path(sys.argv[2]).read_text())
if len(expected) != 10:
    raise SystemExit('Expected exactly ten public providers')
if not all(hashlib.sha256((root/n).read_bytes()).hexdigest()==h for n,h in expected.items()):
    raise SystemExit('Source/provider hash mismatch')
print('Matched public media/SCP providers:',len(expected))
PYCHECK
python3 "$kit/scripts/audit-kernel-exports.py" --image "$image" --system-map "$map" --config "$config" --output-dir "$out/builtin"
mkdir "$out/modules"
for dir in ov8856-standard-balanced-fps ov02a10-standard-fps-range dw9768-upstream mt8183-seninf-dual-highres mt8183-p1-public; do
 work=$out/modules/$dir; cp -R "$kit/modules/$dir" "$work"
 obj=$(sed -n 's/^obj-m *[:+]*= *\([^ ]*\)\.o.*/\1/p' "$work/Makefile")
 make -C "$kernel" M="$work" ARCH=arm64 LLVM=1 -j"$jobs" "$obj.o" > "$work/objects.log" 2>&1
 llvm-nm -u "$work/$obj.o" > "$work/undefined.txt"
 # OV8856 has no private exports; use its freshly compiled ELF as audit input.
 sensor=$out/modules/ov8856-standard-balanced-fps/ov8856.o
 python3 "$kit/scripts/audit-required-exports.py" --builtin-json "$out/builtin/public-kernel-exports.json" --module-root "$providers" --undefined-list "$work/undefined.txt" --sensor-module "$sensor" --sensor-sha "$(sha256sum "$sensor" | cut -d' ' -f1)" --output-dir "$work" > "$work/audit.log"
 make -C "$kernel" M="$work" ARCH=arm64 LLVM=1 KBUILD_EXTRA_SYMBOLS="$work/required-exports.symvers" -j"$jobs" modules > "$work/modules.log" 2>&1
 /sbin/modinfo "$work/$obj.ko" > "$work/modinfo.txt"
 sha256sum "$work/$obj.ko"
done
[ ! -e "$kernel/Module.symvers" ]
