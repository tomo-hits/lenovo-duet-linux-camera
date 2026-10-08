# Known limitations

English | [日本語](KNOWN_ISSUES.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->


The supported target is the original Duet SKU176 with the exact postmarketOS v26.06 / Alpine v3.24 / 6.18.28-mt81 ARM64 KCFI kernel and matching SCP firmware. Results apply to the package versions and environments named in [TESTING](../developer/docs/TESTING.md).

- **Rear brightness:** the source uses gamma 2.4; the earlier v1.0.1 packages use 2.2. A same-binary comparison found improved midtone visibility. The libcamera r103 / meta 0.2.13 APK set has not passed installation/restoration testing. Exposure and gain limits still constrain low-light performance.
- **Frame rate:** the standard request is 1536 × 864 at 30 fps. The v1.0.0 run delivered about 24 fps; a separate gamma-control run measured about 30.01155 fps from received buffers. Neither establishes constant Snapshot display FPS.
- **Sleep:** close the camera first. Active capture intentionally blocks suspend. Closed-camera deep sleep and capture after wake passed on v1.0.1; day-scale durability is untested.
- **Image quality and AF:** complete colour/noise/flicker and distance-dependent autofocus calibration are unfinished. LensPosition is hidden; automatic flicker detection is unavailable.
- **Audio and related functions:** PipeWire excludes BlueZ, JACK SPA, FFmpeg, Vulkan, ROC, libmysofa and EVL. v1.0.1 speaker playback before installation and during front capture passed a listening check; microphone recording and all optional functions are untested. Camera/SCP isolation disables competing hardware codec, MDP and JPEG consumers.
- **Recovery:** retain the kit and saved boot/package state. Restoration refuses failed STOP, unrelated package/world changes and external changes to saved targets. Boot-slot writes are not power-failure atomic; restoration is not a whole-filesystem rollback.
- **Legacy backups:** v1.0.0 package backups lack expected configuration states and cannot be restored automatically by newer scripts. Retain the original kit and backup for inspected recovery; do not install over an active old transaction or use old restoration after external settings edits.
- **Other environments:** other models, kernel builds and independently updated media stacks are unsupported. Historical external-USB tests used older package sets and do not validate the current set.

[Installation and restoration](INSTALL.md) / [developer details](../developer/docs/KNOWN_ISSUES.md) / [kernel porting](../developer/docs/KERNEL_UPDATES.md).
