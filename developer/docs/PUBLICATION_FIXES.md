# Validation and packaging safeguards

English | [日本語](PUBLICATION_FIXES.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->


This describes the pre-S1 v5 validation-helper revision and later S2 packaging checks. Results apply to their recorded versions; [TESTING](TESTING.md) lists subsequent hardware evidence.

## Validation-helper revision

DT compatibility, provider/export and input-hash checks use explicit exceptions that also run under optimized Python. DT output rejects existing files/symlinks; boot assets copy only verified inputs. The boot helper locates adjacent ABI JSON by default.

Independent auditors and host harnesses retain source-compatible SPDX and original third-party notices. Five camera APKs were reissued with the actual PROJECT-MIT notice; accepted libcamera/PipeWire APKs, camera ELF and configuration bytes were retained. [PACKAGE_FIXES](PACKAGE_FIXES.json) records changed files and [PUBLICATION_REVIEW](PUBLICATION_REVIEW.json) records host/native checks. This helper/notice revision has no new camera hardware acceptance. OV02A10 regression fixtures include their GPL source and provenance and run independently of external development directories.

## S2: validated preserved inputs and a fixed replacement path

Checking APK signatures alone did not validate the adjacent `payload-hashes.json`; its paths could influence the P1 destination. The helper now checks the whole sidecar before output creation, including package set, canonical paths, SHA256 syntax and duplicate keys. It pins the preserved sidecar hash to `34efbd109b4b2d3ce72bc327fe3972e35cf23907fe84aa67e32e8e59b9eab88e`.

P1 replacement uses a fixed path inside the package and rejects intermediate/final symlinks and hard links at the destination. Hash, ELF/modinfo checks and packaging use the same captured input bytes. `tests/test-package-paths.py` runs under normal Python and `-O`; [S2_VALIDATION](S2_VALIDATION.json) records fixtures and separate native packaging checks. This change leaves the v6 APK archive and S1 runtime code unchanged and adds no hardware acceptance.
