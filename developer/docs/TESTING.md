# Testing and validation scope

English | [日本語](TESTING.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->


Build success, sensor recognition, image acquisition, assessed image quality and restoration are separate results. Every record applies to its named version and environment. Host fixtures verify software behavior and do not establish hardware compatibility or concurrency.

## v1.0.1 baseline and rear-gamma comparison (2026-10-08)

[ACCEPTANCE_2026-10-08.json](ACCEPTANCE_2026-10-08.json) separates the following tests:

- **v1.0.1:** boot preparation, installation/reboot, 15 minutes of Snapshot capture per camera, speaker listening checks, 28.4427 seconds of closed-camera deep sleep and post-wake capture, refusal of externally modified settings, cancellation of untouched PREPARED boot state, and ordinary restoration followed by reboot/login and saved-target verification passed. Microphone recording and whole-filesystem equality were not assessed.
- **Rear gamma:** seven cases with 2,880 requests used standard Gamma controls on the same v1.0.1 ELF binaries, covering 30→15→30 fps, gamma 2.4→2.2 and front/rear switching. All ended with clean STOP and SCP offline. Rear received-buffer timestamps indicated about 30.01155 fps. A separate same-scene Snapshot comparison loaded the exact candidate YAML and found brighter, easier-to-see rear midtones and no major front abnormality. The original OS was restored and its recorded targets verified after reboot.

The seven-case metadata test did not load the candidate YAML; the visual test did. Received-buffer timing and driver completion counters do not establish Snapshot display FPS. The initial tuning edit passed 31 host cases in both normal Python and `-O` (62 executions). [REAR_BRIGHTNESS_PACKAGING](REAR_BRIGHTNESS_PACKAGING.json) records the separate native signatures, payload/source matching and package checks. **Installation/restoration of the new libcamera r103 / meta 0.2.13 APK set have not been tested.**

## Earlier hardware records

| Version/environment | Results and limits | Record |
| --- | --- | --- |
| v1.0.0, internal eMMC | Front/rear 3,600 frames each, about 24 delivered fps, 20 JPEGs, reuse and scoped sleep/recovery; image quality and audio limits identified separately | [PUBLIC_ACCEPTANCE](PUBLIC_ACCEPTANCE.json) |
| Modules 0.2.6 / meta 0.2.10, internal eMMC | Short Snapshot capture, new-process reuse and exact recorded-target restoration; boot prepared/restored separately; no measured fps, endurance, audio or sleep acceptance | [INTERNAL_ACCEPTANCE](INTERNAL_ACCEPTANCE.json) |
| Modules 0.2.5 / meta 0.2.9, external USB | Canonical-DT cold boot, guarded automatic activation, capture/reuse and restoration of 838 original packages/world/settings; restored USB boot and audible audio untested | [USB_ACCEPTANCE](USB_ACCEPTANCE.json) / [UPDATE_VALIDATION](UPDATE_VALIDATION.json) |
| S1 / v6, RAM | 194 frames in six timing/reuse sessions, early STOP in 0.0645 s and subsequent reuse, two Snapshot processes and five JPEGs; no new boot/endurance/calibration assessment | [S1_VALIDATION](S1_VALIDATION.json) / [LOW_FPS_FIX](LOW_FPS_FIX.md) |
| Fresh v4 and earlier RAM builds | Five-module build, fixed SDK/export checks, ordinary-app capture and signed set rollback/reapplication; earlier long/high-resolution/sleep tests are version-specific | [BUILD_RECORD](BUILD_RECORD.json) / [REAR-FPS](../REAR-FPS.md) / [shared-stack fixes](../UPSTREAM-STATE-FIXES.md) |

## Failure findings and regression coverage

The original USB updater installed modules 0.2.4-r1 but P1's RAM-only check refused loading after reboot; exact package/world/configuration restoration passed. The following guarded USB mode resolved loading. A later first-stream EPERM occurred with zero firmware commands/frames because SCP recovery was enabled; the corrected startup helper verified recovery disabled and capture/reuse passed. [P1_USB_FIX](P1_USB_FIX.json), [EXTERNAL_PREPARATION](EXTERNAL_PREPARATION.json), [SCP_RECOVERY_FIX](SCP_RECOVERY_FIX.json).

[S1](LOW_FPS_FIX.md) fixes an idle publisher timeout at low fps without removing capture progress or STOP deadlines. [S2](PUBLICATION_FIXES.md) validates preserved packaging sidecar inputs and fixes the P1 destination. [Host test guide](../tests/README.md) lists runnable commands and fixture limitations. DT tests require separately compiled stock/active DT inputs; synthetic boot images are not real boot evidence.

## Hardware verification procedure

Close applications normally, confirm STOP/idle, use a complete matched signed package set, and follow the supported update/activation procedure. Verify two sensors with `cam -l`; test front/rear live capture, saved images, switching, normal close and reopening in a new process. Record errors, frame progression, exit status and package/module correspondence. Test ordinary audio, closed-camera sleep and restoration separately. Do not force unload or treat forced reboot as normal resource recovery.

Complete colour/noise/flicker and distance-dependent AF calibration, day-scale durability, other SKUs/kernels and codec/SCP coexistence remain unverified. [Known limitations](../../docs/KNOWN_ISSUES.md).
