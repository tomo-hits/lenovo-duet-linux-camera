#!/bin/sh
# SPDX-License-Identifier: MIT
# Developer build only: produces candidate modules, never installs or loads them.
set -eu

fail() { printf '%s\n' "$*" >&2; exit 1; }
[ "$#" = 4 ] || fail 'usage: build-modules-from-tree.sh KERNEL_BUILD TARGET_RELEASE NEW_OUTPUT JOBS'
kernel=$1; release=$2; out=$3; jobs=$4
kit=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
case "$jobs" in ''|*[!0-9]*) fail 'JOBS must be a positive integer' ;; esac
case "$jobs" in *[1-9]*) ;; *) fail 'JOBS must be a positive integer' ;; esac
case "$release" in ''|*[!A-Za-z0-9._+-]*) fail 'Invalid target kernel release' ;; esac
[ "$(uname -m)" = aarch64 ] || fail 'Use native aarch64 Linux'
[ "$(uname -s)" = Linux ] || fail 'Use native aarch64 Linux'
[ ! -e "$out" ] && [ ! -L "$out" ] || fail 'Output must be a new directory, not a symlink'
kernel=$(CDPATH= cd -- "$kernel" && pwd)
for file in Makefile .config Module.symvers vmlinux include/config/auto.conf \
    include/config/kernel.release include/generated/autoconf.h include/generated/utsrelease.h; do
    [ -s "$kernel/$file" ] || fail "Missing complete kernel build input: $file"
done
[ "$(cat "$kernel/include/config/kernel.release")" = "$release" ] || fail 'Kernel release mismatch'
grep -Fqx "#define UTS_RELEASE \"$release\"" "$kernel/include/generated/utsrelease.h" || fail 'Generated kernel release mismatch'
for option in ARM64 MODULES CC_IS_CLANG LD_IS_LLD CFI; do
    for file in .config include/config/auto.conf; do
        grep -Fqx "CONFIG_$option=y" "$kernel/$file" || fail "Required CONFIG_$option=y missing from $file"
    done
    grep -Fqx "#define CONFIG_$option 1" "$kernel/include/generated/autoconf.h" || fail "Generated CONFIG_$option mismatch"
done
for file in .config include/config/auto.conf; do
    if grep -Eq '^CONFIG_CFI_PERMISSIVE=' "$kernel/$file"; then
        fail 'Permissive CFI is not supported by this build helper'
    fi
done
if grep -Eq '^#define CONFIG_CFI_PERMISSIVE ' "$kernel/include/generated/autoconf.h"; then
    fail 'Generated permissive CFI configuration is not supported'
fi
compiler=$(clang --version | sed -n '1p')
[ -n "$compiler" ] || fail 'Cannot identify clang'
for file in .config include/config/auto.conf; do
    grep -Fqx "CONFIG_CC_VERSION_TEXT=\"$compiler\"" "$kernel/$file" || fail "Clang identity differs from $file"
done
command -v ld.lld >/dev/null
command -v llvm-readelf >/dev/null
command -v sha256sum >/dev/null
command -v modinfo >/dev/null
modinfo --version | grep -F kmod >/dev/null || fail 'Use kmod modinfo'
lld_version=$(ld.lld --version | sed -n 's/^.*LLD \([0-9][0-9]*\)\.\([0-9][0-9]*\)\.\([0-9][0-9]*\).*/\1 \2 \3/p')
set -- $lld_version
[ "$#" = 3 ] || fail 'Cannot identify LLD version'
lld_number=$(($1 * 10000 + $2 * 100 + $3))
for file in .config include/config/auto.conf; do
    grep -Fqx "CONFIG_LLD_VERSION=$lld_number" "$kernel/$file" || fail "LLD version differs from $file"
done
llvm-readelf -h "$kernel/vmlinux" | grep -Eq 'Machine:[[:space:]]+AArch64' || fail 'vmlinux must be an AArch64 ELF'

# Do not inherit MAKEFLAGS, KCFLAGS, KBUILD_MODPOST_WARN, or other overrides.
# The selected build tree and toolchain are trusted developer inputs.
kernel_make() { env -i PATH="$PATH" LC_ALL=C make -C "$kernel" ARCH=arm64 LLVM=1 "$@"; }
[ "$(kernel_make -s kernelrelease)" = "$release" ] || fail 'Kbuild kernel release mismatch'
# Parents may be new, but the final mkdir must fail if any output already exists.
mkdir -p -- "$(dirname -- "$out")"
mkdir -- "$out"
out=$(CDPATH= cd -- "$out" && pwd)
{
    printf 'status=experimental-candidate-only\ntarget_release=%s\ncompiler=%s\n' "$release" "$compiler"
    printf 'kernel_build=%s\nsource_kit=%s\n' "$kernel" "$kit"
    ld.lld --version
    sha256sum "$kernel/.config" "$kernel/Module.symvers" "$kernel/vmlinux"
} > "$out/build-inputs.txt"

for directory in ov8856-standard-balanced-fps ov02a10-standard-fps-range \
    dw9768-upstream mt8183-seninf-dual-highres mt8183-p1-public; do
    work=$out/$directory
    cp -R "$kit/modules/$directory" "$work"
    kernel_make M="$work" clean > "$work/clean.log" 2>&1 || fail "Clean failed; see $work/clean.log"
    kernel_make M="$work" -j"$jobs" modules > "$work/build.log" 2>&1 || fail "Build failed; see $work/build.log"
    case "$directory" in
        ov8856-*) object=ov8856 ;;
        ov02a10-*) object=ov02a10 ;;
        dw9768-*) object=dw9768 ;;
        mt8183-seninf-*) object=mtk_seninf ;;
        mt8183-p1-public) object=mt8183_p1 ;;
    esac
    module=$work/$object.ko
    [ -s "$module" ] || fail "Missing module: $module"
    llvm-readelf -h "$module" | grep -Eq 'Machine:[[:space:]]+AArch64' || fail "Module is not AArch64: $module"
    vermagic=$(modinfo -F vermagic "$module")
    [ "${vermagic%% *}" = "$release" ] || fail "Module vermagic mismatch: $module"
    modinfo "$module" > "$work/modinfo.txt"
    sha256sum "$module" >> "$out/module-sha256.txt"
done
printf '%s\n' 'Built five experimental candidate modules. No installation, activation, or hardware compatibility validation was performed.'
