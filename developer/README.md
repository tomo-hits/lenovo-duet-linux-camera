# Developer guide

Use [INSTALL](../docs/INSTALL.md) for installation/restoration and [TESTING](docs/TESTING.md) for versioned hardware results and limitations. This source includes rear gamma 2.4; installation/restoration of its new APK set remain untested.

English | [日本語](README.ja.md) | [User README](../README.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->

This directory groups drivers, userspace changes, building, packaging, testing, and experiment preparation. For ordinary installation and restoration, use the [user guide](../docs/INSTALL.md).

## Choose a task

| Task | Guide |
| --- | --- |
| Build the current version from source | [Build, sign, and install a candidate](docs/BUILD.md) |
| Support another kernel | [Kernel updates and rebuilding](docs/KERNEL_UPDATES.md) |
| Inspect individual development operations | [Manual installation reference](docs/MANUAL_INSTALL.md) |
| Prepare the hardware experiment's boot environment | [External boot experiment](docs/EXTERNAL_BOOT.md) |
| Review the internal-OS experiment and recovery | [Internal eMMC preparation and recovery](docs/INTERNAL_EMMC.md) |
| Validate a change | [Host regression tests](tests/README.md) / [Hardware testing and results](docs/TESTING.md) |
| Understand the implementation | [Architecture](docs/ARCHITECTURE.md) / [Packaging](packaging/postmarketos/README.md) |
| Review outstanding work | [Known development issues](docs/KNOWN_ISSUES.md) |
| Review source and redistribution obligations | [Provenance](docs/PROVENANCE.md) / [OSS distribution](docs/OSS.md) / [Licenses](../LICENSE.md) |
| Assemble a downloadable update | [Distribution archive](docs/REPACKING.md) |
| Inspect earlier versions | [Release history](docs/RELEASE_NOTES.md) / [Historical repacking](docs/REPACKING.md) |

## From build to validation

1. Prepare native ARM64 Alpine and the fixed source/kernel inputs.
2. Follow [BUILD](docs/BUILD.md) to build five modules, libcamera/PipeWire, and signed APKs.
3. On the matching test environment, pass the package directory and independently verified signing-key fingerprint to the root `install.sh`.
4. Follow [TESTING](docs/TESTING.md), recording build, recognition, acquisition and image quality separately, and verify restoration.

In these guides, `kit` is the absolute path to **this `developer/` directory**. `repo` is its parent containing `install.sh`:

```sh
repo=/absolute/path/to/lenovo-duet-linux-camera
kit="$repo/developer"
```

| Directory | Contents |
| --- | --- |
| `modules/` | Five current modules and earlier implementations needed for comparison/regression testing |
| `patches/`, `tuning/` | libcamera/PipeWire and other patches, and tuning data |
| `configs/` | Target kernel configuration and pinned inputs |
| `scripts/` | Build, packaging, verification and update implementations |
| `packaging/` | Compatibility, DT/boot integration and activation helpers |
| `tests/` | Host regression tests and inputs |
| `docs/` | Procedures, design, provenance and versioned validation records |
| `LICENSES/` | Original license texts |

The current distributable P1 is `modules/mt8183-p1-public/`. Its module README preserves earlier USB history; current internal-mode results are in [TESTING](docs/TESTING.md). Other P1 directories preserve earlier implementations and comparison tests. Historical JSON manifests/input hashes describe their recorded versions; unless stated otherwise, their source paths are relative to `developer/`. Do not mix older archives with the new layout.

## Development constraints

The public target is SKU176 / exact 6.18.28-mt81 ARM64 KCFI and matching SCP firmware. The public internal-eMMC route includes stock-boot preparation and restoration; it does not support arbitrary OS layouts. Validate different kernels and hardware separately. Preserve the original state and a recovery route before experiments. Before writing media, verify its model, capacity, serial and mounts, and save needed data on a separate medium.

Keep passwords, PINs, signing private keys and camera photographs out of distributions. Authentication uses the operator's terminal; photographs stay local. Record original authors, sources, licenses and the target kernel.
