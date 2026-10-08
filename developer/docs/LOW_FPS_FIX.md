# Low-FPS publisher fix (S1)

English | [日本語](LOW_FPS_FIX.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->

The side review found that the P1 publisher's fixed 1500 ms idle wait could stop a valid rear-camera stream at 2 fps. The first late publication needs SOF5, at least 2000 ms after SOF1 at that frame rate. An initial copy can arrive before this failure; the symptom is premature STOP/error retention and blocked normal reuse.

`dph_run()` now waits for publication or explicit closure. An idle publisher owns no pending copy. Capture retains its bounded SOF/firmware progress waits and calls `dph_close()` on STOP/error; closure wakes both completions and the existing worker join remains required. The 1500 ms deadline for an active copy in `dph_send()` is unchanged. Sensor, ISP and userspace algorithms are unchanged.

## Build and signed packages

The P1 subtree for that revision was freshly compiled with native ARM64 Alpine clang 22.1.3 against the already matched, prepared Linux 6.18.28-mt81 tree. The 125 selected SDK files/config and ten providers were checked; all 201 required exports resolve and KCFI is enabled. This revision did not repeat a full clean kernel or userspace build. Exact input hashes and the P1 module SHA256 are in [S1_VALIDATION.json](S1_VALIDATION.json).

The v6 signed repository has modules **0.2.4-r0**, meta **0.2.6-r0/r1**, config **0.1.3-r0/r1**, libcamera **0.7.2-r102** and PipeWire/GStreamer **1.6.8-r103**. Three APKs were reissued; eight v5 APKs are byte-identical. Only the P1 `.ko` regular payload changed; meta dependencies were updated. All eleven APK signatures and the index were verified. [S1_PACKAGE_FIXES.json](S1_PACKAGE_FIXES.json) preserves correspondence to [PRE_S1_PACKAGES.json](PRE_S1_PACKAGES.json). See [BUILD](BUILD.md) for fresh-source and preserved-input packaging recipes.

## Actual hardware acceptance

Tests used the existing original Duet SKU176 RAM boot and legacy DT alias. All 203 regular files in the eight installed runtime packages match the signed v6 payloads, and the loaded P1 build-ID note matches the new ELF.

| Camera / request | Completed frames | Measured median fps |
| --- | ---: | ---: |
| Rear 2 fps | 48 | 2.00002 |
| Rear 2 fps reopen | 12 | 2.00002 |
| Rear 850000 µs frame duration | 32 | 1.17650 |
| Rear 30 fps | 60 | 30.01291 |
| Front 15 fps | 30 | 15.01659 |
| Rear 2 fps after early STOP | 12 | 2.00002 |

The six finite sessions completed 194 frames. Each ended normally with capture/input errors zero, inputs idle, finalized, no pending copies/references and SCP offline. A normal SIGINT at SOF1 before the first late publication completed in **0.0645 seconds**, followed by successful 2 fps reuse.

Two ordinary Snapshot sessions used distinct processes, confirmed front/rear live progress, made three switches and saved five local JPEGs, then closed normally with exit 0. PipeWire/WirePlumber stayed running between those sessions. Photos remain only on the device's RAM filesystem and are excluded from public artifacts; calibrated visual quality was not accepted by these tests.

Final state: camera apps closed, capture/input errors zero, inputs idle, finalized, pending/references zero, SCP offline. The eMMC OS partition stayed read-only; the whole disk was not read-only. No eMMC OS, boot region or firmware was written.

## Host regression and limits

```sh
python3 tests/test-handoff-wait.py
python3 modules/mt8183-p1-public/tests/run_stop_gate.py
```

The actual handoff functions pass seven ASan/UBSan cases: short/2000 ms/4350 ms startup, idle STOP, retained ownership after the active deadline, and closed/busy admission. The existing fifteen STOP/gate cases also pass. The host timing shim is a regression oracle, separate from the hardware measurements above.

Canonical DT boot, external OS cold boot, endurance, sleep, high-resolution, codec coexistence and complete AF/colour/noise/flicker calibration were not repeated. Earlier results remain historical in [TESTING](TESTING.md).
