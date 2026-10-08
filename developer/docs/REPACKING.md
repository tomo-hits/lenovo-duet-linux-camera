# Build a distribution archive (maintainers)

English | [日本語](REPACKING.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->

## Rear-gamma package set (v1.0.2)

This candidate repackages six APKs as libcamera `0.7.2-r103` with rear gamma 2.4 and meta `0.2.13-r0/r1`, retaining the other five APKs byte-identically. Use the new [package manifest](PUBLIC_PACKAGES.json) and [complete-source record](COMPLETE_SOURCES.json); the previous [package record](PRE_REAR_BRIGHTNESS_PACKAGES.json) and [source record](PRE_REAR_BRIGHTNESS_COMPLETE_SOURCES.json) are preserved. Do not overwrite earlier APKs, archives or release assets.

Provide the distribution archive, external SHA256SUMS and a manifest identifying its source/package correspondence. Verify contents, signatures and hashes and offer the matching source together in the same release. Check [Releases](https://github.com/tomo-hits/lenovo-duet-linux-camera/releases) for available downloads.

[The hardware record](ACCEPTANCE_2026-10-08.json) covers the same ELF binaries and candidate YAML. Installation and restoration of the newly signed APK set remain untested and are separate from archive verification.

## Current update archive

The user downloads one archive containing `install.sh`, `restore.sh`, their supporting code, the signed packages, and corresponding source. Source ZIP downloads alone contain no APKs. Assemble a new archive after the source checksums have been refreshed and the native packager has verified the APK signatures:

```sh
repo=/absolute/path/to/lenovo-duet-linux-camera
kit="$repo/developer"
python3 "$kit/scripts/build-update-bundle.py" \
 --packages /absolute/path/to/verified-packages \
 --complete-sources /absolute/path/to/verified-complete-sources.tar.xz \
 --output /absolute/path/to/new-camera-update.tar
```

The assembler copies only source entries in the repository-root `SHA256SUMS` and selected package/source files. It rejects changed hashes, unsafe paths, mismatching key identity and existing output files. It copies no private signing key or package staging directory. The default author key also requires exact agreement with the current `PUBLIC_PACKAGES.json`; `USB_PACKAGES.json` remains the earlier USB record. Bundle metadata names the checked manifest and records `hardware_acceptance: false`: the assembler verifies archive contents and does not run a hardware test. The previous [PUBLIC_ACCEPTANCE](PUBLIC_ACCEPTANCE.json) is retained separately from [the follow-up checks](ACCEPTANCE_2026-10-08.json). Matching signed bytes alone does not establish installation acceptance of the new APK set. A developer-signed build must pass `--expected-key-sha256` with an independently verified SPKI fingerprint, as in [BUILD](BUILD.md). That fingerprint is also required when installing a developer-signed bundle.

`BUNDLE.json` records source/package scope and `BUNDLE_SHA256SUMS` covers every bundled file except itself. Publish an external archive SHA256 alongside the archive. Archive assembly does not replace APK signature verification or hardware acceptance. The root checksum manifest must describe the intended source revision; historical JSON input hashes retain their version-specific meaning, with their paths relative to `developer/`.

## Preserved packaging recipes


These historical recipes require separately preserved inputs and their corresponding signing key. They are not the installation or fresh-build route. Start with [BUILD](BUILD.md) to build the current source, or [INSTALL](MANUAL_INSTALL.md) to use an already compatible system. Set `kit` to the repository's `developer/` directory and `work` to a new output directory before using an example below.

## Package the publication-review revision

The preserved pre-S1 v5 used this separate recipe to verify signed v4 inputs and update only DT helpers/notices in five camera APKs. Every ELF, configuration payload and six userspace APKs stay intact; originals are not overwritten. Use package-fresh.py in BUILD for a new build from source. Original archive SHA256: `ce5f22c79a8c22a700b5721f1efca0de40b7f1a16280c0921653cb720385ed39`.

```sh
python3 "$kit/scripts/package-release-fixes.py" \
 --accepted-repository /absolute/path/to/extracted-fresh-v4 \
 --output "$work/review-packages" \
 --sign-key /private/local-signing-key.rsa \
 --public-key /public/pmos@local-6ab3c683.rsa.pub
```

## Package the S1 revision from preserved v5 inputs

The supplied v6 uses a fresh compile of all current P1 C/H files against the verified prepared kernel, then replaces only that ELF in the signed v5 inputs. This narrower recipe is optional; the fresh kernel/userspace recipe in BUILD remains the route for a full source build. Verify the preserved v5 archive SHA256 `96ec65037ad726a6e339fb6520e611adb8491ddd22643460b5dcf2ac2c5825b4` before extracting it. The script pins all eleven v5 package hashes and the public key, allows only the P1 payload change, and verifies all new signatures/index. It rejects existing output paths. Signing requires the corresponding local private key; keys are not bundled.

The S2 revision also validates the complete `payload-hashes.json` sidecar before creating output: exact package set, canonical relative paths, hash syntax and duplicate-key rejection. Its preserved v5 SHA256 is pinned to `34efbd109b4b2d3ce72bc327fe3972e35cf23907fe84aa67e32e8e59b9eab88e`. P1 is written only to the fixed module path inside the extracted package; intermediate/final symlinks and a hard-linked P1 destination are rejected. Hash, ELF, modinfo and packaging checks use the same captured P1 bytes. See [S2 validation](S2_VALIDATION.json). This packaging-helper revision leaves the supplied v6 APKs and the S1 hardware evidence unchanged; it adds no new hardware acceptance.

The module path below is produced by build-kernel.sh. The displayed P1 hash is the supplied native build's value; a fresh build can differ because of debug paths/toolchains, so pass the hash measured from your own verified output. See [S1 validation](S1_VALIDATION.json) for exact source inputs and [low-FPS fix](LOW_FPS_FIX.md) for RAM hardware evidence.

```sh
python3 "$kit/scripts/package-low-fps-fix.py" \
 --accepted-repository /absolute/path/to/extracted-fresh-v5 \
 --p1-module "$work/kernel/modules/mt8183-p1-public/mt8183_p1.ko" \
 --p1-sha256 81bfb2020f3e5f8a0a6f3a1435ba1b751db06b439db3620db91e28d98e375d41 \
 --output "$work/s1-packages" \
 --sign-key /private/local-signing-key.rsa \
 --public-key /public/pmos@local-6ab3c683.rsa.pub
```
