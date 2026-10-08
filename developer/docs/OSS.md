# Source distribution and component notices

English | [日本語](OSS.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->


## Current source/package correspondence

[PUBLIC_PACKAGES](PUBLIC_PACKAGES.json) records modules **0.2.8-r0**, config **0.1.3-r0/r1**, meta **0.2.13-r0/r1**, libcamera **0.7.2-r103** and PipeWire/GStreamer **1.6.8-r104**. [COMPLETE_SOURCES](COMPLETE_SOURCES.json) identifies `mt8183-camera-complete-sources-20261008-v1.tar.xz` and its hash. The rear gamma 2.4 tuning matches source, APK payload and complete source. [REAR_BRIGHTNESS_PACKAGING](REAR_BRIGHTNESS_PACKAGING.json) records native package checks. Packaging evidence and hardware installation acceptance are separate; see [TESTING](TESTING.md).

## Materials required with binaries

Provide the matching repository source kit and complete-source archive together with signed APKs and checksums. The source kit includes all current module code, fixed kernel configuration/patches, userspace patches, build/install/package scripts and tuning. The complete-source archive includes full patched libcamera/PipeWire inputs, build scripts and upstream licenses, original Linux 6.18.28 source, and tuning overrides. [PROVENANCE](PROVENANCE.md) identifies sources and component licenses. A source kit from a different package revision is not a substitute for matching source.

Maintain equivalent access to corresponding source while offering binaries. See [GPLv2 section 3](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html), [LGPLv2.1 section 4](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html) and [GNU's source-distribution explanation](https://www.gnu.org/licenses/gpl-faq.en.html#DistributingSourceIsInconvenient). Preserve per-file copyright/SPDX, all original license texts, libcamera COPYING.rst, PipeWire COPYING and REUSE metadata. The root license guide does not replace component licenses. APK payloads include actual component notices and the P1 RAW-packing MIT notice.

[MODIFICATIONS](MODIFICATIONS.json) records changed code and original build-input hashes. Dated modification comments in distribution source identify edits; they do not by themselves establish a new build or test. Public guides/translations are CC0-1.0, excluding original third-party material.

## Official zstd supplement

The internal-OS toolkit can include the unchanged official Alpine v3.24 `main/aarch64` **zstd 1.5.7-r2** APK under `packages/official-zstd/`, separately from the 11 camera APKs. It decompresses the existing SCP firmware for read-only inspection. The updater may add this pinned package only when absent and its runtime dependencies are already satisfied. Preserve its original signature and bytes.

[OFFICIAL_ZSTD](OFFICIAL_ZSTD.json) records APK/index hashes, the upstream [v1.5.7 source](https://github.com/facebook/zstd/archive/v1.5.7.tar.gz) and fixed [Alpine recipe](https://github.com/alpinelinux/aports/blob/3c6e2ee2b16f403d53eab39c4426eb61f003c322/main/zstd/APKBUILD). Include the original [LICENSE](../LICENSES/ZSTD-1.5.7-LICENSE.txt), [COPYING](../LICENSES/ZSTD-1.5.7-COPYING.txt) and [source notices](../LICENSES/ZSTD-1.5.7-NOTICES.txt). This distribution selects BSD-3-Clause for dual-licensed Zstandard and preserves the separate BSD-2-Clause zstdgrep and MIT divsufsort notices. zstd source is separate from the camera complete-source archive.

## Historical records

Earlier manifests identify their own package sets: [INTERNAL_PACKAGES](INTERNAL_PACKAGES.json), [USB_PACKAGES](USB_PACKAGES.json), [PACKAGES](PACKAGES.json) and [PRE_REAR_BRIGHTNESS_PACKAGES](PRE_REAR_BRIGHTNESS_PACKAGES.json). Their evidence does not validate another version. Use matching historical source when redistributing old binaries. Generated signing/IPA private keys, device firmware/kernel/OS images, credentials and camera captures are not distribution materials.
