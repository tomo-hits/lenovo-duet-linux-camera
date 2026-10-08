# Development limitations

English | [日本語](KNOWN_ISSUES.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->


The current source/package mapping is [PUBLIC_PACKAGES](PUBLIC_PACKAGES.json), [PUBLIC_P1_BUILD](PUBLIC_P1_BUILD.json) and [COMPLETE_SOURCES](COMPLETE_SOURCES.json). [TESTING](TESTING.md) separates current tuning checks from earlier versioned hardware results; the new r103 / meta 0.2.13 APK set has not passed installation/restoration testing.

P1 releases initial runtime-PM references asynchronously. Closed-camera suspend retires idle unpublished state or a stopped session only after its required ownership checks. Active capture refuses suspend; resume uses the existing CAM OFF→ON validation. Activation verifies platform binding and a video endpoint as well as modprobe success.

Normal close retains stopped DMA for reuse. Failed STOP retains DMA and refuses reuse. Forced unload, forced reference release and forced application termination are not recovery procedures. Kernel ABI and firmware checks are fixed. Concurrent codec/SCP use, arbitrary hot unplug, other models/kernels and independent media-stack upgrades are unsupported or unverified. User WirePlumber policy can override standard configuration.

Fresh builds may differ in bytes because of paths, debug information and a newly generated IPA key. Verify each build's signatures and source/payload correspondence separately. Complete image-quality/AF calibration and long-term durability remain unfinished. [User limitations](../../docs/KNOWN_ISSUES.md) / [kernel porting](KERNEL_UPDATES.md).
