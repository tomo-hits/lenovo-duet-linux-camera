# Install and restore

English | [日本語](INSTALL.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->

**Distribution:** use a complete signed update archive listed in [GitHub Releases](https://github.com/tomo-hits/lenovo-duet-linux-camera/releases). If no archive is listed, build a signed kit with the [developer guide](../developer/docs/BUILD.md). The source tree alone is not an installable kit. The rear-gamma package set (libcamera r103 / meta 0.2.13) has not passed installation/restoration testing.

v1.0.1 introduced external-setting protection during restoration and recovery of untouched boot-preparation transactions. Backups made by v1.0.0 lack expected config states and cannot be automatically restored by v1.0.1. Keep the original kit and backups for reviewed recovery; do not apply v1.0.1 over an active v1.0.0 transaction. Do not use the old restore script after editing saved targets. See [known limitations](KNOWN_ISSUES.md).

## Supported system

The installation toolkit targets the original Lenovo Duet **SKU176**, postmarketOS **v26.06 / Alpine v3.24**, **6.18.28-mt81 / ARM64 / KCFI**, on internal eMMC. It preserves the installed OS and kernel. The scripts check the exact kernel, SCP firmware and boot layout and refuse other configurations.

Requires curl, Python 3, sudo, apk-tools 3, vbutil_kernel and the ChromiumOS developer signing keys provided by the OS. Connect power and avoid concurrent OS/package updates. Photos stay on the device.

## 1. Download from GitHub and verify

Choose an available signed release, replace the tag placeholder below, and check its asset filenames. Run in a terminal on the Duet. If curl is missing, first install it with `sudo apk add curl`.

```sh
version=REPLACE_WITH_AVAILABLE_RELEASE_TAG
mkdir "duet-camera-$version"
cd "duet-camera-$version"
curl -fL --retry 3 -o "lenovo-duet-camera-${version}.tar" \
  https://github.com/tomo-hits/lenovo-duet-linux-camera/releases/download/${version}/lenovo-duet-camera-${version}.tar
curl -fL --retry 3 -o SHA256SUMS \
  https://github.com/tomo-hits/lenovo-duet-linux-camera/releases/download/${version}/SHA256SUMS
sha256sum -c SHA256SUMS && tar -xf "lenovo-duet-camera-${version}.tar"
cd lenovo-duet-linux-camera
```

Require every checksum to report `OK` before extracting. GitHub's “Source code” download and a clone do not contain the signed packages. The update kit includes install/restore scripts, signed packages and corresponding source.

## 2. Prepare the stock boot configuration once

Close camera applications and run from the extracted folder:

```sh
sudo sh install.sh --prepare-boot --check
sudo sh install.sh --prepare-boot
sudo systemctl reboot
```

The script saves the complete 32 MiB kernel partition, boot assets, fstab and camera isolation settings, including hashes and permissions, before integrating the camera DT. It preserves the kernel, initramfs and command line. It does not change firmware, GPT or hardware boot0/boot1 regions. A single-slot write is not power-failure atomic. Retain the saved backups and an independently verified OS recovery backup on another computer.

Skip this preparation if this kit has already prepared the boot configuration.

## 3. Install the camera update

After reboot, log in normally and return to the extracted folder:

```sh
sudo sh install.sh
sudo systemctl reboot
```

The installer preserves original package files, versions, world and affected settings before verifying and installing the update. Internet access is required to retrieve original packages missing from the local cache. The desktop user is normally detected; use `--user NAME` only when it cannot be selected.

Open Snapshot after reboot to capture from both cameras. The default request is 1536 × 864 at 30 fps; achieved rate depends on exposure and processing. Close the camera before sleeping; active capture intentionally blocks suspend. See [scope and limitations](KNOWN_ISSUES.md).

## 4. Restore

Close the camera and run from the same folder. This also restores a boot-only preparation:

```sh
sudo sh restore.sh
sudo systemctl reboot
```

The script restores saved packages, world and affected settings, then the kernel partition, boot assets, fstab and isolation settings. It checks saved bytes, permissions, links and original absence. Photos and personal documents remain. Restoration covers recorded targets rather than the entire filesystem.

Retain `/var/lib/duet-camera-update`, `/var/lib/duet-camera-boot-preparation` and `/var/lib/duet-camera-boot-transaction` through installation and restoration, and as long as needed for recovery.

## Errors

Keep the error output. Do not bypass compatibility checks. Unrelated OS/package upgrades or changes to saved targets may cause restoration to refuse overwrites. Failed STOP diagnostics must not be bypassed with forced unload, force-kill or repeated SCP shutdown. If the OS cannot boot or saved targets no longer match, recover using the independently saved OS backup.

The prepared external USB path remains an advanced `--external-usb` option. v1.0.0 hardware acceptance targets internal eMMC.
