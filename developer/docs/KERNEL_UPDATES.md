# Kernel updates and rebuilding

English | [日本語](KERNEL_UPDATES.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->

**For users: apply an update set that supports your OS and kernel, then reboot.** Follow [INSTALL](../../docs/INSTALL.md) for the commands and current validation status. Rebuilding is a maintainer task; users do not need to build the modules to install a supported set.

The current target is one specific build of **6.18.28-mt81 / ARM64 / SKU176**, with matching DT and SCP firmware. Until an update set supports a newer kernel, keep the working external environment on its supported kernel. A distribution rebuild can change compatibility even when the kernel release string stays the same. This project does not provide automatic rebuilding after an OS update.

Keep a bootable copy of the working **external** environment before testing an OS/kernel update. It must include the kernel, DTB, firmware, camera packages and configuration; `/etc/apk/world` alone is insufficient. If an update breaks the camera, boot that preserved environment and run the [manual compatibility check](MANUAL_INSTALL.md) and camera checks. The camera update's file/package restore cannot undo a separate OS/kernel update.

## Maintainer workflow

**Complete target kernel build → five camera modules → matching packages and ABI record → external hardware test.** This is a porting procedure for future kernels, not a claim that they already work. For reproducing the exact supported 6.18.28 build, use [BUILD](BUILD.md).

### 1. Complete the target kernel build

Use the target distribution's kernel source, patches, configuration and build recipe. Build both the kernel and its modules with the matching native ARM64 Clang/LLD toolchain. Retain:

- `.config`, generated headers and `include/config/kernel.release`;
- `vmlinux` and the genuine `Module.symvers` from that complete build;
- the media, videobuf2, remoteproc and MediaTek SCP components required by the drivers.

The helper below requires native aarch64 Linux, KCFI enabled, permissive CFI disabled, and the same Clang identity and LLD version recorded by the kernel build. It uses `make`, `clang`, `ld.lld`, `llvm-readelf`, `sha256sum` and kmod's `modinfo`. It does not select or download a kernel or toolchain.

A headers package or `modules_prepare` alone is insufficient: `modules_prepare` does not generate `Module.symvers`. See the [Linux external module build documentation](https://docs.kernel.org/kbuild/modules.html). The prepared 6.18.28 SDK deliberately uses a separate audited-export method without `Module.symvers`; follow BUILD for that SDK. Do not substitute an old `Module.symvers` into a new kernel build.

### 2. Build the five camera modules

Set these paths and replace `YOUR_TARGET_KERNEL_RELEASE` with the intended kernel release, including its suffix. Run as a normal build user in the matching native ARM64 environment. The output directory must be new.

```sh
repo=/absolute/path/to/lenovo-duet-linux-camera
kit="$repo/developer"
kernel_build=/absolute/path/to/complete-kernel-build
target_release=YOUR_TARGET_KERNEL_RELEASE
work=/absolute/path/to/new-rebuild-root
sh "$kit/scripts/build-modules-from-tree.sh" \
 "$kernel_build" "$target_release" "$work/kernel/modules" 3
```

The helper copies the module sources into the output directory and builds OV8856, OV02A10, DW9768, SENINF and P1 with Kbuild. It records `build-inputs.txt`, `module-sha256.txt`, and each module's `build.log` and `modinfo.txt`. It does not install or load the modules.

If a kernel API, exported symbol or compiler check fails, port the affected source and rebuild into a new output directory. Do not ignore modpost failures, disable KCFI or force-load the result. A successful compile and matching `vermagic` are not hardware acceptance.

### 3. Package a matched update set

**The current packager and activation code are fixed to 6.18.28-mt81. They require manual changes before they can package a different target.** Do not pass new-kernel candidates directly to the unchanged `package-fresh.py`.

| File or component | Review for the new target |
| --- | --- |
| `scripts/package-fresh.py` | Module install directory, package versions and dependency pins |
| `scripts/apply-update.py` | Package versions in `PINS`; pass an independently verified custom signing-key fingerprint through `--expected-key-sha256` |
| `packaging/postmarketos/kernel-abi.json` | Kernel release, actual kernel notes/Image hashes, validated SCP firmware and board |
| `packaging/postmarketos/duet-camera-activate.py` | Kernel release passed to `depmod`; retain the USB/eMMC checks behind `--external-usb` and P1's default-off `external_usb` opt-in |
| `packaging/postmarketos/camera-nodes.json` and DT helpers | Camera graph and bindings used by the new kernel |
| External startup and update helpers | Module names, SCP initialization and compatibility checks |
| `configs/kernel/` and `docs/SOURCE_INPUTS.json` | Source revision, distribution patches/configuration, toolchain and input provenance |

Take hashes from the actual new build and test boot, preserving the previous target's records. If extending the pinned BUILD method, review its input verifier, hash sets and export auditors too; changing expected hashes alone is insufficient. Reuse userspace binaries only when their media interfaces and distribution dependencies remain compatible; otherwise rebuild them through BUILD.

After adapting the packaging code and checking its inputs, use BUILD's signed packaging procedure to create a separate candidate set. Include its public verification key, package hashes, matching module/userspace source, build instructions and notices. Keep signing private keys local. Preserve original SPDX/copyright headers and licenses, and update [PROVENANCE](PROVENANCE.md) and [MODIFICATIONS.json](MODIFICATIONS.json) for source changes. See [OSS](OSS.md) for the corresponding-source materials to distribute with binaries.

### 4. Test and release for that kernel

Use a separate external USB test environment and leave the internal eMMC OS, boot areas and firmware unchanged. First boot the new target kernel and verify its DT/firmware and compatibility record. Then apply the candidate update set, reboot, and check:

1. The intended kernel and all five camera modules are active; both sensors are recognized.
2. Front/rear live view, photos, switching, normal close and reopening work in the normal camera application; SCP returns to its expected idle state.
3. Normal audio still works, and a further reboot preserves camera operation.
4. The update's restore procedure returns the environment to its pre-install state.

Record **build success, sensor recognition, image acquisition and image quality separately**; keep camera images local. Use [TESTING](TESTING.md) for detailed checks and existing limits. Publish the supported kernel build/OS, update-set hashes, source revision and test results together only after these checks pass.

No newer-kernel build or hardware acceptance is established by this guide. The build helper's host tests check its command handling and input rejection only.
