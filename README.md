# Linux cameras for the original Lenovo Duet Chromebook

English | [日本語](README.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->


Camera drivers and an installation/restoration toolkit for the original Lenovo IdeaPad Duet Chromebook **SKU176**, targeting **postmarketOS v26.06 / Alpine v3.24 / 6.18.28-mt81 / ARM64 / KCFI**. Applications such as Snapshot use the front and rear cameras through libcamera and PipeWire.

## Installation

A signed update archive is required. Check [GitHub Releases](https://github.com/tomo-hits/lenovo-duet-linux-camera/releases) for available downloads, then follow the [installation and restoration guide](docs/INSTALL.md). A clone or GitHub's automatic source archive contains source only and cannot install the camera update by itself.

From a verified, extracted update kit, prepare the boot configuration once, reboot, and install:

```sh
sudo sh install.sh --prepare-boot
sudo systemctl reboot
# After reboot, return to the same extracted directory.
sudo sh install.sh
sudo systemctl reboot
```

To restore the saved packages, settings and boot configuration:

```sh
sudo sh restore.sh
sudo systemctl reboot
```

Keep the kit and its backups throughout installation and recovery. See the guide for prerequisites, checks and legacy-backup compatibility.

## Source and validation

This source includes rear-camera gamma 2.4 for brighter midtones while retaining the 30 fps request. The corresponding libcamera r103 / meta 0.2.13 package set has packaging checks and a same-binary tuning comparison; **installation and restoration of that new APK set have not been tested**. See [versioned test results](developer/docs/TESTING.md).

The tested v1.0.1 baseline passed front/rear capture, speaker playback, sleep recovery and restoration of recorded OS targets. Frame rate depends on exposure and processing. Complete image-quality/autofocus calibration, long-term durability, other kernels and concurrent hardware codec use remain unverified. [Known limitations](docs/KNOWN_ISSUES.md).

## Development and licenses

[Build, packaging and test guides](developer/README.md) include fixed sources and component provenance. [License guide](LICENSE.md) / [copyright notices](NOTICE.md). Original component licenses are retained.
