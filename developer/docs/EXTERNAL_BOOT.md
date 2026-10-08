# Prepare an external USB boot experiment

English | [日本語](EXTERNAL_BOOT.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->

This is a developer preparation record for reproducing the hardware experiment. It is not part of ordinary update-kit installation. It assumes the pinned pmbootstrap and pmaports trees are already obtained and configured; it is not an automated setup from an empty host. **If the matching USB already boots, use the verified kit’s `install.sh --external-usb` for the file/package update; do not rebuild its OS image for each camera update.** USB boot, canonical DT/kernel/firmware matching and eMMC read-only protection have passed. The historical modules 0.2.5 / meta 0.2.9 candidate fixes P1's RAM-only startup refusal through guarded external-USB activation. Its native build/package checks, actual USB application, cold boot, automatic activation and front/rear device listing pass. Front/rear capture and new-process reuse also passed. See [USB_ACCEPTANCE](USB_ACCEPTANCE.json) for restoration and audible-audio status.

## One supported input set

Use SKU176, postmarketOS v26.06 / Alpine v3.24, Phosh with systemd, PipeWire audio and wpa_supplicant Wi-Fi. The exact kernel is 6.18.28-mt81; `prepare-external-rootfs.py` checks the uncompressed Image and logical SCP firmware hashes against `kernel-abi.json`. The distribution can supply `scp.img.zst`; install `zstd` in the rootfs. Other kernel builds are rejected even if the version string matches.

The preparation used pmbootstrap 3.11.1, commit `edb3097c7307216b088478b7c424ee07d636f41b`, and pmaports commit `368093c7a882637ee00d32932fb0dafd24cfc4d4`. Upstream package repositories can change; preserve the inputs and stop on hash mismatch. The kernel and firmware remain distribution inputs, not bundled project binaries. Camera module sources, licenses and original authors are listed in [OSS](OSS.md).

## Prepare the distribution rootfs

On a native ARM64 Linux builder, use an isolated pmbootstrap work directory and the unmodified pinned pmbootstrap source. Configure `google-kukui`, the OS/UI/providers above, user `camera`, SSH-key import disabled, boot size 512 MiB and extra space 2048 MiB. Include Python 3, OpenSSL, kmod, util-linux, zstd, Snapshot and libcamera-tools. Keep the recovery media separate.

Run the wrapper in an operator terminal. It sends the password directly to the rootfs's `passwd` utility; it avoids upstream's temporary plaintext password file and never accepts password arguments. Root is locked and sshd is disabled by this invocation. The OS retains its normal password hash. Do not store passwords in command arguments, environment variables or scripts.

```sh
kit=/absolute/path/to/lenovo-duet-linux-camera/developer
pmb_tree=/absolute/path/to/pmbootstrap
work=/absolute/path/to/new-pmbootstrap-work
config=/absolute/path/to/pmbootstrap.cfg
python3 "$kit/scripts/pmbootstrap-install-direct-passwd.py" \
 --pmbootstrap-tree "$pmb_tree" -- -c "$config" -w "$work" install \
 --no-sshd --no-local-pkgs --no-recommends
```

The wrapper accepts image/rootfs installation only. Physical-disk, rsync and flasher modes are rejected. Its optional `--prepare-locked-account` mode requires an already locked account and leaves login unconfigured; such an image needs direct operator password setup before use. This mode was used during the automated preparation check, not as a login test.

## Integrate camera boot files into a new image

Shut down pmbootstrap chroots using the pinned tool before inspecting its completed image. Do not use `pmbootstrap chroot --image`: this image has ChromeOS kernel/boot/root partitions p1/p2/p3, while that command assumes p1/p2 for boot/root. The following inspection attaches only the regular image file read-only and detaches it before the builder starts. Confirm the image path in your work directory.

```sh
stock="$work/chroot_native/home/pmos/rootfs/google-kukui.img"
test -f "$stock" && test ! -L "$stock" || exit 1
loop=$(sudo losetup --find --show --read-only --partscan "$stock")
root_uuid=$(sudo blkid -s UUID -o value "${loop}p3")
boot_uuid=$(sudo blkid -s UUID -o value "${loop}p2")
sudo losetup -d "$loop"
python3 "$kit/packaging/postmarketos/prepare-external-rootfs.py" \
 --rootfs "$work/chroot_rootfs_google-kukui" \
 --root-uuid "$root_uuid" --boot-uuid "$boot_uuid" \
 --output /absolute/path/to/new-overlay
sudo python3 "$kit/scripts/apply-external-overlay-to-image.py" \
 --image "$stock" --prepared /absolute/path/to/new-overlay \
 --output /absolute/path/to/new-camera.img
```

The overlay contains the canonical camera DT, explicit root/boot UUIDs, an early initramfs guard, camera alias blacklists and a systemd activation unit. The builder copies the stock image into a new file, mounts only that copy, rebuilds mkinitfs/FIT, verifies the kernel signature, checks its partition size and runs filesystem checks after unmounting. It never accepts a physical device as image input. Preserve the stock image for comparison and the pre-camera protected image for recovery.

The guard requires unique UUID matches, both partitions on the same USB disk, and read-only flags on detected eMMC disk/partition/boot devices before allowing root mounting. Failure stops startup. This protection passed isolated fixtures and the initial Duet boot check. It does not change eMMC contents, firmware or GBB.

## Write and test only after identifying the USB

These helpers intentionally do not select or erase a USB. Before writing, record its model, capacity, serial and mount state; verify that all current partitions belong to the intended test medium and that their contents are safely backed up. Recheck it immediately before writing. A recovery USB and the internal eMMC are not experiment targets. Keep the prepared pre-camera image on separate storage; after setting its login it contains private OS authentication data and must not be published.

After USB boot, verify the running kernel/DT, the USB root and eMMC read-only flags, then follow [INSTALL](../../docs/INSTALL.md) with a complete verified package set and the external-USB mode. The activation service skips a rootfs without camera packages. On later boots it uses `duet-camera-activate --external-usb`, which verifies USB root/boot and internal eMMC read-only protection before opting P1 into external-USB operation. Capture front/rear photos locally, close/reopen Snapshot, check SCP returns offline, reboot and repeat. Check ordinary audio as well. Recognition, image acquisition and acceptable picture quality are separate results.

For file/package restoration, use INSTALL. The historical modules 0.2.5 candidate also restored all 838 original packages, world and saved settings on the real USB; post-restore boot remains unverified. Restoring the complete protected pre-camera image to the same verified test USB remains a fallback if package restoration is unavailable, and that full-image recovery route is untested. Removing the meta-package alone does not restore libcamera/PipeWire. Do not alter internal boot settings or firmware.

With a split initramfs, upstream first reads initramfs-extra from the explicit boot UUID using a read-only mount. The guard runs in stage 2 before root or a writable boot is mounted; it does not precede that initial read-only boot access.

## Initial USB boot finding (2026-10-06)

The device booted from USB with the canonical camera graph, matched kernel/firmware, and every eMMC disk, partition and boot area read-only. No camera packages had been installed at that first boot. Stock codec/MDP consumers and remoteproc auto-boot left SCP running, so the pre-install offline gate refused installation.

The dedicated external OS fix candidate blocks codec/MDP/JPEG and SCP autoload, then explicitly registers only the SCP provider before the desktop starts. One normal shutdown returns the single auto-boot reference created by the matched kernel registration. It requires offline afterward and rejects a provider already loaded, competing modules/holders, a repeated attempt, or a retained running state. It never drains references or forces driver removal. Hardware codec/MDP/JPEG functions are consequently unavailable in this dedicated test OS.

The SCP fix passed on the next USB boot. The updater then installed the earlier modules 0.2.4-r1 candidate and verified 206 runtime files. On reboot, P1 refused to load because its existing safety check accepted only RAM roots. Restoring that candidate returned the original 838 packages/world/settings.

The historical modules 0.2.5-r0 candidate adds a default-off `external_usb` parameter. The activation helper enables it only after verifying the prepared USB root/boot and internal eMMC read-only state. It was rebuilt and repackaged without generating a new kernel or boot image; see [P1_USB_FIX](P1_USB_FIX.json). The preserved pre-camera image is unchanged. The new candidate has since passed USB application, cold boot, automatic guarded activation and front/rear device listing, with all 206 runtime files verified after reboot. Front/rear capture and new-process reuse also passed. See [USB_ACCEPTANCE](USB_ACCEPTANCE.json) for recovery and audible-audio status.
