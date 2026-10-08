#!/bin/sh
# Modified on 2026-10-05: publication review fixes; see docs/PUBLICATION_REVIEW.json.
# SPDX-License-Identifier: MIT
# Fresh checkout/build; installs only into OUTPUT/stage.
set -eu
[ "$#" = 2 ] || { echo 'usage: build-userspace.sh OUTPUT JOBS' >&2; exit 2; }
out=$1; jobs=$2
[ "$(uname -m)" = aarch64 ]
[ "$(gcc -dumpfullversion)" = 15.2.0 ]
export CC=gcc CXX=g++
kit=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
[ ! -e "$out" ]; mkdir -p "$out"
out=$(CDPATH= cd -- "$out" && pwd)
git init "$out/libcamera"
git -C "$out/libcamera" remote add origin https://git.libcamera.org/libcamera/libcamera.git
git -C "$out/libcamera" fetch --depth=1 origin c0049ea0605c1492c99b8f82bb04661a04cf1bf1
git -C "$out/libcamera" checkout --detach FETCH_HEAD
git -C "$out/libcamera" apply --check "$kit/patches/libcamera-0.7.2-duet-complete.patch"
git -C "$out/libcamera" apply "$kit/patches/libcamera-0.7.2-duet-complete.patch"
python3 - "$out/libcamera" "$kit/libcamera-source-hashes.json" <<'PYSOURCE'
import hashlib,json,sys
from pathlib import Path
root=Path(sys.argv[1]); expected=json.loads(Path(sys.argv[2]).read_text())
if not all(hashlib.sha256((root/n).read_bytes()).hexdigest()==h for n,h in expected['files'].items()):
    raise SystemExit('libcamera source hash mismatch')
print('Matched accepted libcamera changed/new source files:',len(expected['files']))
PYSOURCE
cp "$kit/.tarball-version" "$out/libcamera/"
meson setup "$out/libcamera/build" "$out/libcamera" --buildtype=release --prefix=/usr --libdir=lib -Dpipelines=simple -Dipas=softisp -Dcam=enabled -Dqcam=disabled -Dgstreamer=disabled -Dtest=false -Ddocumentation=disabled -Dpycamera=disabled -Dlc-compliance=disabled -Dandroid=disabled -Dv4l2=disabled
ninja -C "$out/libcamera/build" -j"$jobs"
DESTDIR="$out/stage" meson install -C "$out/libcamera/build"
install -m644 "$kit"/tuning/*.yaml "$out/stage/usr/share/libcamera/ipa/softisp/"
git init "$out/pipewire"
git -C "$out/pipewire" remote add origin https://gitlab.freedesktop.org/pipewire/pipewire.git
git -C "$out/pipewire" fetch --depth=1 origin b741e0c74f5436f0c925f7741140db0efd32cf4e
git -C "$out/pipewire" checkout --detach FETCH_HEAD
[ "$(git -C "$out/pipewire" rev-parse HEAD)" = b741e0c74f5436f0c925f7741140db0efd32cf4e ]
git -C "$out/pipewire" rev-parse HEAD > "$out/pipewire-upstream-commit.txt"
for name in pipewire-1.6.8-libcamera-complete.patch pipewire-1.6.8-gst-copy.patch pipewire-gst-state-change-backport-v2.patch pipewire-link-reset-preparing-upstream-v2.patch pipewire-gst-real-copy-default-v3.patch; do
 git -C "$out/pipewire" apply --check "$kit/patches/$name"
 git -C "$out/pipewire" apply "$kit/patches/$name"
done
export PKG_CONFIG_PATH="$out/stage/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$out/stage"
# Scope the staged sysroot to libcamera; other dependencies use normal /usr.
sed -i "s|^prefix=/usr|prefix=$out/stage/usr|" "$out/stage/usr/lib/pkgconfig/"*.pc
unset PKG_CONFIG_SYSROOT_DIR
meson setup "$out/pipewire/build" "$out/pipewire" --buildtype=release --prefix=/usr --libdir=lib -Dlibcamera=enabled -Dgstreamer=enabled -Dlibsystemd=disabled -Dlogind=disabled -Ddocs=disabled -Dman=disabled -Dtests=disabled -Dexamples=disabled -Dsession-managers=[] -Djack=disabled -Dvulkan=disabled -Dbluez5=disabled -Dffmpeg=disabled -Droc=disabled -Dlibmysofa=disabled -Devl=disabled
ninja -C "$out/pipewire/build" -j"$jobs"
DESTDIR="$out/stage" meson install -C "$out/pipewire/build"
# Restore redistributable pkgconfig paths.
sed -i "s|^prefix=$out/stage/usr|prefix=/usr|" "$out/stage/usr/lib/pkgconfig/libcamera"*.pc
