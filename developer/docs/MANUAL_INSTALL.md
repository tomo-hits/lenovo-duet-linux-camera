# Manual installation reference for developers

English | [日本語](MANUAL_INSTALL.ja.md) | [Developer guide](../README.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->

**Version scope:** the APK commands below are for the v1.0.2 package set or a fresh build from this branch, using libcamera `0.7.2-r103` and meta `0.2.13-r0/r1`. For an ordinary public v1.0.1 installation, use that kit’s bundled `install.sh` and the [user guide](../../docs/INSTALL.md). Installation/restoration of the new candidate APK set remain untested.

Use the root `install.sh` / `restore.sh` for ordinary installation and restoration. See the [user guide](../../docs/INSTALL.md) and [BUILD](BUILD.md) for passing custom signed packages.

This reference describes individual compatibility, signature and APK operations for development investigation. **It does not automatically save or restore the pre-install state.** Use it only after independently preparing a backup and recovery route.

### What you need

| Requirement | Supported value |
| --- | --- |
| Device | Original Lenovo IdeaPad Duet Chromebook, `google,krane-sku176` |
| System | Dedicated external postmarketOS v26.06 / Alpine v3.24, ARM64 musl |
| Kernel | The exact **6.18.28-mt81** build; the same version string alone is insufficient |
| Firmware and device tree | Matching SCP firmware and an already active camera graph; checked below |
| Package tools | apk-tools 3, Python 3, OpenSSL and kmod |
| Camera session | WirePlumber **0.5.15-r0**, PipeWire, a desktop camera portal and Snapshot |
| Recovery | A separate, bootable recovery route and a restorable backup of the external OS |

The packages replace the system's **libcamera and PipeWire libraries**. The included PipeWire build disables BlueZ, JACK, FFmpeg, Vulkan, ROC, libmysofa and EVL. Existing audio and optional desktop features have not been accepted with this replacement. Use a dedicated test system. See [known issues](KNOWN_ISSUES.md).

For signed package availability, check [Releases](https://github.com/tomo-hits/lenovo-duet-linux-camera/releases); follow the [user installation guide](../../docs/INSTALL.md) for the supported internal OS. The manual operations below are for a prepared external test OS. Use a verified v1.0.2 candidate with the versions above, or create a signed package directory with [BUILD](BUILD.md) from this branch. Earlier v1.0.1 packages do not match the version pins below. Cloning this repository provides source, not the required kernel, firmware, OS or APKs. A source build's output directory (`$work/packages`) has the layout used below.

### 1. Check the system before installing anything

Run the following as root on the external test OS so the check can read its device tree. Replace both paths with your local source checkout and extracted package directory. Keep them available through all steps.

```sh
kit=/absolute/path/to/lenovo-duet-linux-camera/developer
candidate=/absolute/path/to/packages
python3 "$kit/packaging/postmarketos/duet-camera-check.py" \
 --abi "$kit/packaging/postmarketos/kernel-abi.json"
```

Continue only if the command exits successfully and reports `"compatible": true`. This check reads the running kernel build, board, device tree and firmware; it does not install packages, load modules or change boot files. Do not edit the recorded hashes to make a mismatch pass. For a newer kernel, follow [kernel updates and rebuilding](KERNEL_UPDATES.md).

Before changing the external OS, make and verify its backup using that OS's recovery procedure. Keep the backup on separate storage. Preserve its boot assets, installed package binaries and versions, `/etc/apk/world`, repository settings, and system/user PipeWire and WirePlumber settings. A copy of `world` or a list of versions alone cannot restore replaced libraries. In historical external-USB tests, one earlier candidate restored the original packages/world/settings; another candidate also matched all original 838 packages, world and saved settings on restore. Post-restore USB boot was unverified in that record. Those results are not hardware acceptance of the v1.0.2 candidate.

### 2. Verify the package source

For a supplied archive, verify its SHA256 against the separately obtained distribution manifest **before extracting it**. Then verify the signing key's fingerprint and the APK signatures. The key path below is for the supplied bundle; for your own build, replace the filename with the public key you supplied to the packaging command:

```sh
public_key="$candidate/keys/pmos@local-6ab3c683.rsa.pub"
openssl pkey -pubin -in "$public_key" -outform DER | sha256sum
apk verify --keys-dir "$candidate/keys" "$candidate"/repo/aarch64/*.apk
```

The supplied development key's SPKI DER SHA256 must be:

```text
94d05c05d71e63aa74b0a2f11a4f4e3d8138e701daf5fe4f95e980b8fef73cd9
```

For your own signed build, set `public_key` to the public key you supplied to the packaging command and compare against your own independently recorded fingerprint. Do not use the supplied fingerprint for a different key. Stop if the fingerprint or any signature check fails; do not bypass verification with `--allow-untrusted`.

### 3. Install the matched set

Close all camera applications normally, then stop PipeWire and WirePlumber using your desktop's service controls. Keep both stopped until step 4.

This first-install path requires the camera drivers to be unloaded. Check:

```sh
lsmod | grep -E '^(mt8183_p1|duet_p1_video_raw|mtk_seninf|ov02a10|ov8856|dw9768) '
cat /sys/class/remoteproc/remoteproc0/state
```

The first command must show no matching module; the second must show `offline`. If a driver is already loaded, close applications normally and reboot into the prepared external environment without automatic camera activation before using this manual path. Never force-unload a driver after an active stream or failed STOP. Installing an APK cannot replace a module already running in memory.

The version pins below are **only for the v1.0.2 package set or this branch’s fresh build**, selecting **1536 × 864 at 30 fps**. Preview the transaction first; inspect it for unexpected OS/kernel changes or unrelated package removals. The metapackage selects the matching drivers, configuration and userspace components; the explicit library versions also replace any existing world-file pins.

```sh
install -m0644 "$public_key" /etc/apk/keys/ &&
apk --repository "$candidate/repo/aarch64/packages.adb" add --simulate \
 duet-camera=0.2.13-r1 libcamera=0.7.2-r103 libcamera-ipa=0.7.2-r103 \
 pipewire-libs=1.6.8-r104 gst-plugin-pipewire=1.6.8-r104
```

The first command above trusts the verified key; [apk's simulation](https://github.com/alpinelinux/apk-tools/blob/v3.0.8/doc/apk.8.scd) does not install packages. If the proposed transaction is acceptable, install and activate:

```sh
apk --repository "$candidate/repo/aarch64/packages.adb" add \
 duet-camera=0.2.13-r1 libcamera=0.7.2-r103 libcamera-ipa=0.7.2-r103 \
 pipewire-libs=1.6.8-r104 gst-plugin-pipewire=1.6.8-r104 &&
duet-camera-check &&
duet-camera-activate --external-usb
```

For 15 fps, use `duet-camera=0.2.13-r0` in **both** package commands. Lower frame rates allow longer exposures in dark scenes. Package installation and activation do not write the kernel, boot files or firmware. External activation checks that root/boot share one USB and every internal eMMC device is read-only, then verifies compatibility and SCP idle state before loading P1 with `external_usb=1`. The parameter defaults to false; the startup service supplies it through this helper. Activation does not replace old loaded drivers.

### 4. Confirm that both cameras work

Restart your normal user's PipeWire/WirePlumber session with WirePlumber's standard `main` profile. Existing overrides in `/etc/wireplumber` or your user configuration can override the supplied camera settings. Do not run the camera app as root.

1. Run `cam -l` as the desktop user and confirm that **two cameras** are listed. If permission is denied, fix device access through the desktop session's normal `video`/udev policy; do not make the devices world-writable.
2. Open Snapshot, check front and rear live images, and take a photo with each. Keep the photos local.
3. Close Snapshot normally, reopen it and repeat camera switching. After the final close, confirm SCP returns to `offline`.

Listing cameras establishes recognition; taking photos establishes image acquisition. Neither proves calibrated image quality. If a camera fails, preserve the error message and follow [testing](TESTING.md) and [known issues](KNOWN_ISSUES.md). Do not repeatedly force-reload the modules.

### Updates and recovery

Read [kernel updates and rebuilding](KERNEL_UPDATES.md) **before upgrading the kernel or camera packages**. The current packages have no automatic rebuild service. Do not mix independently upgraded libcamera, PipeWire and camera modules.

If installation or camera use fails, close applications normally and stop the session managers. If the driver cannot stop cleanly, leave the modules loaded and use the external OS's recovery route. Restore the external OS backup, including its original package set, configuration and matching boot assets, before resuming normal use. Removing `duet-camera` alone does **not** restore the replaced libcamera/PipeWire libraries.

Active or failed sessions reject suspend. Current internal-mode code retires normally stopped or idle unpublished state before sleep; external manual installation of this APK set has not passed sleep testing. See [known development issues](KNOWN_ISSUES.md).

### Package versions and scope

This branch’s fresh recipe produces modules 0.2.8-r0 / meta 0.2.13-r0/r1 / libcamera 0.7.2-r103 and PipeWire/GStreamer 1.6.8-r104. The targeted v1.0.2 repack uses the same versions. Earlier v1.0.1 and preserved v6 APKs cannot be used with the pins above. These version pins identify a package set; they do not establish downloadable release availability. Compressed SCP firmware is verified through zstd decompression without rewriting it. PipeWire's pulse-server modules remain owned by the matching distribution pipewire-pulse package; audio from this new candidate’s manual external installation remains untested, separately from the v1.0.1 internal speaker check.
