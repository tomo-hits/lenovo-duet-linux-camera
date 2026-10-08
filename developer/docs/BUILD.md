# Build from fixed source

The rear-gamma v1.0.2 package set corresponds to [PUBLIC_PACKAGES](PUBLIC_PACKAGES.json); [PRE_REAR_BRIGHTNESS_PACKAGES](PRE_REAR_BRIGHTNESS_PACKAGES.json) preserves the previous public set. P1 reuses the ELF recorded in [PUBLIC_P1_BUILD](PUBLIC_P1_BUILD.json), with 203 audited exports. See the [previous acceptance](PUBLIC_ACCEPTANCE.json) and [current validation scope](ACCEPTANCE_2026-10-08.json) separately. The USB/S1 paragraphs below preserve earlier results.


English | [日本語](BUILD.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->

The fresh build recipes below create new work directories and staged files. They do not deploy to a device, copy accepted camera binaries, use a GUI rootfs payload, or build a replacement kernel Image. Run on native **aarch64 Linux / Alpine v3.24 musl** with sufficient disk (about 8GB free). The validation builder used Alpine clang/LLD **22.1.3**, KCFI, GCC **15.2**, Meson **1.11.1**, Samurai **1.2** (Ninja-compatible version **1.9**), GStreamer **1.28.3** and apk-tools **3.0.8**. Check the release build record for exact installed package versions.

This recipe reproduces the **6.18.28-mt81** camera packages. It requires three stages: prepare the kernel inputs and build five modules, build libcamera/PipeWire, then create signed APKs. For a newer kernel, use [KERNEL_UPDATES](KERNEL_UPDATES.md).

## Working paths

`repo` is the repository checkout and `kit` is its developer source directory. Run subsequent examples in the same shell. Replace `/inputs` and `/new` with your input/extraction paths and use new output directories.

```sh
repo=/absolute/path/to/lenovo-duet-linux-camera
kit="$repo/developer"
work=/absolute/path/to/new-build-root
```

## Dependencies

Install the native compiler and build dependencies from trusted Alpine/postmarketOS repositories. For an Alpine environment, use `eudev-dev`; for the tested postmarketOS systemd environment, use its existing `systemd-dev` provider of libudev. Do not install `libudev-zero-dev` alongside systemd. Native build dependencies include:

```sh
apk add build-base clang llvm lld bison flex perl openssl-dev xz zstd kmod \
 git meson samurai pkgconf python3 py3-jinja2 py3-yaml py3-ply \
 libevent-dev yaml-dev gnutls-dev elfutils-dev libdrm-dev \
 gstreamer-dev gst-plugins-base-dev alsa-lib-dev dbus-dev \
 libsndfile-dev readline-dev ncurses-dev sbc-dev
```

Use kmod's `/sbin/modinfo`, not BusyBox modinfo. `build-kernel.sh` requires Alpine clang 22.1.3. Pin the matching compiler packages if repository defaults change; newer versions are not silently accepted.

## Kernel source and ABI inputs

Download `https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.28.tar.xz`. If you already have the complete-source archive listed in [COMPLETE_SOURCES.json](COMPLETE_SOURCES.json), it contains the identical original tarball. Signed update archives include matching packages and complete source; check [Releases](https://github.com/tomo-hits/lenovo-duet-linux-camera/releases) for availability. See the [download instructions](../../docs/INSTALL.md). Its complete SHA512 is checked by the script and recorded in `configs/kernel/APKBUILD`. The seven included postmarketOS patches are applied in the APKBUILD order. The script supplies `matched.config`, runs `olddefconfig modules_prepare`, requires the config to stay byte-identical and compares 125 selected SDK source files against the accepted SDK. Generated objects/headers come from this fresh tree; the old prepared SDK is not copied.

Obtain the exact kernel APK and APKINDEX from [the postmarketOS v26.06/aarch64 repository](https://mirror.postmarketos.org/postmarketos/v26.06/aarch64/). The matching APK embeds a `pmos@local-6a18a82e.rsa.pub` signature, which is not trusted by the standard keys. Verify the official `build.postmarketos.org.rsa.pub` signed index, its APK control checksum and the control’s SHA256 data checksum with the included verifier; use the trusted repository public key installed by your distribution. The whole APK SHA256 is pinned to `7729c6db0bff84ace07dd55e6f67afc8c3f82982989528d0a055631afb5afd4d`. The following commands are chained so extraction and Image creation run only after successful verification:

```sh
python3 "$kit/scripts/verify-kernel-input.py" \
 linux-postmarketos-mediatek-mt81-6.18.28-r0.apk APKINDEX.tar.gz \
 /etc/apk/keys/build.postmarketos.org.rsa.pub &&
apk extract --allow-untrusted --destination /new/kernel-input \
 linux-postmarketos-mediatek-mt81-6.18.28-r0.apk &&
gzip -dc /new/kernel-input/boot/vmlinuz > /new/kernel-input/Image
```

Here `--allow-untrusted` bypasses only the unrecognized embedded APK key after the independent signed-index/control/data verification has succeeded. It does not waive input verification. The package supplies Image, `boot/System.map`, `boot/config` and `usr/lib/modules/6.18.28-mt81`. All ten pinned public module providers were checked against a separate download. Preserve module relative paths and `.ko.zst` files. Check:

| Input | SHA256 |
| --- | --- |
| Uncompressed Image | cce4914459d15558b54640f360e0a58476edfa74595fe30143d29dce7049d5a1 |
| System.map | 3757e93b5d3cfc162350534b637810fbc0807fed47b644cfc888bd372c664329 |
| Public boot config | 780e607deea636e4ffba18ac49be4b36926d0c2e1308d380d6cb764c604d6756 |
| Prepared matched.config | 141ececb5afef42ba36a5794c4cf0508562b5187d32cf6bc0ab2a9c6d2cd80f4 |

```sh
sh "$kit/scripts/build-kernel.sh" "$work/kernel" \
 /inputs/linux-6.18.28.tar.xz /new/kernel-input/Image \
 /new/kernel-input/boot/System.map /new/kernel-input/boot/config \
 /new/kernel-input/usr/lib/modules/6.18.28-mt81 3
```

`Module.symvers` remains absent from the fresh SDK. `audit-kernel-exports.py` reads actual ARM64 PREL32 builtin export records from Image and validates System.map values, GPL class and namespaces. `audit-required-exports.py` reads actual public media/SCP ELF export tables. CRC zero is allowed only because this exact public config disables MODVERSIONS. Required exports are passed with `KBUILD_EXTRA_SYMBOLS`; no fabricated kernel symvers or ignored missing export is used. OV8856's freshly compiled ELF is checked to have no private sensor exports. The current internal-mode P1 uses 203 audited exports. Historical USB and S1 builds used 202 and 201, respectively; none were missing in their recorded checks. Audit reports and logs remain in the output.

## Userspace source and patch order

```sh
sh "$kit/scripts/build-userspace.sh" "$work/userspace" 3
```

libcamera is fetched at immutable commit `c0049ea0605c1492c99b8f82bb04661a04cf1bf1`. Apply only `libcamera-0.7.2-duet-complete.patch`, copy `.tarball-version`, then use the included Meson options (Simple pipeline, SoftISP IPA, cam enabled). The patch contains all four new AF/flicker/statistics files. Compare the patched source with `libcamera-source-hashes.json`. Install all three tuning YAMLs. The build generates a new IPA signing key internally and installs its matching signed IPA; never export that private key.

PipeWire 1.6.8 is fetched at `b741e0c74f5436f0c925f7741140db0efd32cf4e` and receives these patches in order:

1. `pipewire-1.6.8-libcamera-complete.patch`
2. `pipewire-1.6.8-gst-copy.patch`
3. `pipewire-gst-state-change-backport-v2.patch`
4. `pipewire-link-reset-preparing-upstream-v2.patch`
5. `pipewire-gst-real-copy-default-v3.patch`

PipeWire systemd/logind integration is explicitly disabled to match the accepted eudev RAM runtime; the dependency uses `cmd:udevadm` for either supported udev provider. Standard `client.conf` and `client.conf.avail` defaults are packaged with the client libraries. The complete core, enabled SPA plugins and GStreamer plugin are rebuilt against the freshly staged libcamera and actual target GStreamer ABI. The staging prefix is used only during build; redistributable libcamera pkgconfig files are restored to `/usr`. qcam is excluded from these APKs; its preserved high-resolution test used the optional separately built GUI tool.

## Signed APK packaging

The v1.0.2 candidate uses `0.7.2-r103` for the four libcamera packages with rear gamma 2.4, and `0.2.13-r0/r1` for the two meta packages with fixed dependencies. The other five APKs remain byte-identical to the earlier v1.0.1. Do not redistribute changed tuning under the old r102 names and versions. [package-rear-brightness.py](../scripts/package-rear-brightness.py) narrowly repackages verified previous packages, retaining ELF binaries while changing tuning, versions and dependencies. The fresh-build route below also compiles ELF binaries; it does not automatically inherit byte identity or hardware acceptance from the reused binaries. Verify signatures, hashes and complete corresponding source; installation of the new APK set remains untested.

Use apk-tools 3.0.8 with `mkpkg`/`mkndx`. Supply an existing local RSA signing key and its public key explicitly. There is no recipe that exports the private key. Example paths below are placeholders, not bundled keys:

```sh
python3 "$kit/scripts/package-fresh.py" \
 --kernel "$work/kernel" --userspace "$work/userspace" \
 --output "$work/packages" \
 --sign-key /private/local-signing-key.rsa \
 --public-key /public/local-signing-key.rsa.pub
```

Output: 11 signed current APKs, signed `packages.adb`, public key and package SHA256 manifest. Fresh versions are modules **0.2.8-r0** / meta **0.2.13**, config **0.1.3**, libcamera **0.7.2-r103** and PipeWire/GStreamer **1.6.8-r104**. This recipe packages fresh staged ELF files and current public helpers. It builds from staged source outputs rather than extracting earlier package payloads. Do not publish `userspace/libcamera/build/src/ipa-priv-key.pem`, work trees, provider kernel inputs or signing keys.

The historical USB P1 was rebuilt with native Clang/LLD 22.1.3 and KCFI using the unchanged 125-file SDK and ten providers. The resulting module is 203,704 bytes; its hash and source correspondence are in [P1_USB_FIX.json](P1_USB_FIX.json). The targeted repack changes only P1 and the activation helper in the modules APK plus the two meta-package dependencies; the other eight APKs stay byte-identical. Eleven APKs/index and all 288 regular payload files were verified. No replacement kernel or boot image was built for this fix.

Build success alone does not establish sensor recognition, image acquisition or good image quality. The historical modules 0.2.5 candidate passed USB installation, cold boot, automatic activation and front/rear device listing; front/rear capture and new-process reuse also passed, while complete image-quality calibration remains unfinished. See [TESTING](TESTING.md).

The complete-source archive includes full patched libcamera/PipeWire, their build scripts and original licenses, and the original Linux tarball. Distributed changed code has dated notice comments; [MODIFICATIONS](MODIFICATIONS.json) and the archive’s MODIFICATIONS.json preserve original build-input hashes. S1 changes P1 idle-publication waiting; sensor/ISP algorithms and the complete userspace source are unchanged. See [OSS](OSS.md).

## Install a custom build

Obtain and independently retain the fingerprint from the trusted local public key used for building. Do not treat an unverified key included in a received package bundle as its own trust anchor.

```sh
openssl pkey -pubin -in /public/local-signing-key.rsa.pub -outform DER | sha256sum
```

The output is `$work/packages`. On the validated target environment, place the packages and repository and explicitly supply the **independently verified SPKI DER SHA256 of your own signing public key**. Replace the fingerprint placeholder below with that value. It is distinct from the distribution key.

```sh
sudo sh "$repo/install.sh" --packages "$work/packages" \
 --expected-key-sha256 YOUR_INDEPENDENTLY_VERIFIED_SPKI_SHA256
```

Installation checks this fingerprint, APK signatures and the environment, then saves the original packages/settings before applying the update. Restart after success as instructed. Use the root shell entry point to restore:

```sh
sudo sh "$repo/restore.sh"
```

Use `--user NAME` to specify the desktop user explicitly. Restoration uses the saved originals and does not require passing the custom candidate's key again. See [user installation and restoration](../../docs/INSTALL.md). Building the source alone does not prepare the target boot environment.

Use the [manual installation reference](MANUAL_INSTALL.md) when investigating individual APK operations, [kernel updates](KERNEL_UPDATES.md) for a different target, or [historical repacking](REPACKING.md) to reproduce earlier revisions from preserved inputs.

Candidate signature verification, complete payload re-extraction, byte reuse of 63 ELF files and host checks are recorded in [REAR_BRIGHTNESS_PACKAGING](REAR_BRIGHTNESS_PACKAGING.json).
