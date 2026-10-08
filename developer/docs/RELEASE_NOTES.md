# Version history

English | [日本語](RELEASE_NOTES.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->


These notes describe software versions and their validation. Check [Releases](https://github.com/tomo-hits/lenovo-duet-linux-camera/releases) for actual downloadable artifacts; a historical version mentioned here does not guarantee a current download.

## Rear brightness source: v1.0.2 package set (2026-10-08)

Adds rear OV8856 Adjust gamma 2.4 for brighter midtones, retaining front tuning, AE, default contrast 1.0 and the 30 fps request. Four libcamera APKs use `0.7.2-r103`; two meta APKs use `0.2.13-r0/r1`. Five other APKs and all 63 ELF payloads match v1.0.1. Complete-source tuning, signed-index identities and payload attributes were checked. [Packaging record](REAR_BRIGHTNESS_PACKAGING.json).

Same-binary Gamma-control metadata checks completed seven cases/2,880 requests with clean STOP/SCP offline. A separate same-scene candidate-YAML comparison improved rear visibility without a major front abnormality. These checks do not establish Snapshot display FPS or full calibration. **The new APK set's installation/restoration remain untested.** [Dated hardware results](ACCEPTANCE_2026-10-08.json).

## Recovery safeguards: v1.0.1 (2026-10-07)

Configuration restoration checks contents, permissions, ownership, links, absence and apk world before writing. Interrupted transactions accept only recorded before/package/runtime/restore states. Untouched PREPARED boot transactions can be cancelled/retried after media, original partition, saved-target, package and backup verification, keeping all backups.

The 11 camera APKs, public key, metadata and complete source retain their v1.0.0 bytes (modules 0.2.8-r0 / meta 0.2.12-r1). At this revision, host tests passed 133 cases in each Python mode; the separate 2026-10-08 hardware follow-up passed installation, capture, audio, sleep and restoration. [TESTING](TESTING.md).

v1.0.0 package backups lack expected setting states and newer scripts refuse their automatic restoration. Retain the original kit/backups for inspected recovery; do not install over an active old transaction or use the old restore script after external settings edits.

## Integrated internal installation: v1.0.0 (2026-10-07)

Added matched internal-eMMC boot preparation, signed-package installation and recorded-target restoration. Modules 0.2.8-r0 / meta 0.2.12-r1. Front/rear capture, reuse, scoped sleep and restoration passed; delivered rate was about 24 fps under a 30 fps request. [PUBLIC_ACCEPTANCE](PUBLIC_ACCEPTANCE.json) records this version's exact limits.

## Internal-mode development: modules 0.2.6 / meta 0.2.10 (2026-10-06)

Added explicit internal mode for a separately prepared boot environment. Short ordinary-app capture/reuse, clean STOP, package restoration and separate boot/configuration restoration passed. No integrated boot-preparation tool existed in this version. [INTERNAL_ACCEPTANCE](INTERNAL_ACCEPTANCE.json).

## External USB development: modules 0.2.5 / meta 0.2.9 (2026-10-06)

Added guarded external-USB P1 activation and corrected SCP recovery initialization. Canonical-DT cold boot, automatic activation, camera capture/reuse and original package/world/configuration restoration passed. Restored USB boot and audible audio remain untested. [USB_ACCEPTANCE](USB_ACCEPTANCE.json).

## Earlier build and packaging work (2026-10-05–06)

- External preparation added regular-image boot integration, UUID/eMMC initramfs protection and direct password setup. Distribution-owned Pulse modules were excluded to avoid package conflicts. [EXTERNAL_PREPARATION](EXTERNAL_PREPARATION.json).
- S1 fixed the low-fps idle publisher wait; scoped RAM capture/STOP/reuse passed. [LOW_FPS_FIX](LOW_FPS_FIX.md).
- S2 validates the full preserved-input sidecar and restricts P1 replacement to a fixed path. [PUBLICATION_FIXES](PUBLICATION_FIXES.md).
- Source-build and newer-kernel helpers, shell entry points, complete-source materials and component notices are described in the [developer guide](../README.md). Host tests of a build helper do not establish support for a new kernel.

Original component licenses apply. Distribution details are in [OSS](OSS.md); current limits are in [KNOWN_ISSUES](../../docs/KNOWN_ISSUES.md).
