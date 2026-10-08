#!/bin/sh
# SPDX-License-Identifier: MIT
# postmarketOS initramfs hook for this external-USB-only experiment.
# Never mounts storage. Failure stalls boot before the root or a writable boot can be mounted.
guard_stop() {
    echo "DUET_EXTERNAL_GUARD_FAILED: $*" >&2
    while :; do sleep 3600; done
}
test -n "${root_uuid:-}" || guard_stop 'missing root UUID'
test -n "${boot_uuid:-}" || guard_stop 'missing boot UUID'
test "$root_uuid" != "$boot_uuid" || guard_stop 'identical boot/root UUIDs'
test -z "${root_path:-}${boot_path:-}" || guard_stop 'path override is forbidden'

# BLKROSET changes the running kernel's access policy, not data on the eMMC.
# Set each existing disk/partition read-only and verify its kernel RO flag.
protect_mmc() {
    mmc_found=0
    for disk in /sys/block/mmcblk[0-9]*; do
        test -e "$disk/device/type" || continue
        test "$(cat "$disk/device/type")" = MMC || continue
        mmc_found=1
        name=${disk##*/}
        for entry in "$disk" "$disk"/"$name"p* /sys/block/"$name"boot*; do
            test -e "$entry/ro" || continue
            node=/dev/${entry##*/}
            /sbin/blockdev --setro "$node" || guard_stop "cannot protect $node"
            test "$(cat "$entry/ro")" = 1 || guard_stop "$node remains writable"
        done
    done
}

found=0
for attempt in $(seq 1 30); do
    protect_mmc
    root_nodes=$(blkid -t "UUID=$root_uuid" -o device)
    boot_nodes=$(blkid -t "UUID=$boot_uuid" -o device)
    if test -n "$root_nodes" && test -n "$boot_nodes" && test "$mmc_found" = 1; then
        found=1
        break
    fi
    sleep 1
done
test "$found" = 1 || guard_stop 'USB UUIDs or eMMC did not appear'
test "$(printf '%s\n' "$root_nodes" | wc -l)" -eq 1 || guard_stop 'duplicate root UUID'
test "$(printf '%s\n' "$boot_nodes" | wc -l)" -eq 1 || guard_stop 'duplicate boot UUID'
root_sys=$(/bin/busybox readlink -f "/sys/class/block/${root_nodes##*/}")
boot_sys=$(/bin/busybox readlink -f "/sys/class/block/${boot_nodes##*/}")
test -f "$root_sys/partition" || guard_stop 'root is not a partition'
test -f "$boot_sys/partition" || guard_stop 'boot is not a partition'
case "$root_sys" in */usb[0-9]*/*) ;; *) guard_stop 'root is not USB storage';; esac
case "$boot_sys" in */usb[0-9]*/*) ;; *) guard_stop 'boot is not USB storage';; esac
test "${root_sys%/*}" = "${boot_sys%/*}" || guard_stop 'boot/root are on different USB disks'
test "$root_sys" != "$boot_sys" || guard_stop 'boot/root are the same partition'
protect_mmc
test "$mmc_found" = 1 || guard_stop 'eMMC disappeared before mounting root'
echo "DUET_EXTERNAL_GUARD_OK: root=$root_nodes boot=$boot_nodes"
