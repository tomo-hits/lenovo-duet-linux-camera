# Host regressions

English | [日本語](README.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->

Run from the source-kit root with Python and Clang/cc plus ASan/UBSan. The three DT commands need a compiled stock SKU176 DTB and, where requested, a local active DTB. They never connect to hardware. OV02A10 regression tests use the bundled historical GPL fixture and its provenance.

```sh
cd /absolute/path/to/lenovo-duet-linux-camera/developer
python3 tests/test-boot-update.py
python3 -O tests/test-boot-update.py
python3 tests/test-probe-idle.py
python3 tests/test-update-storage.py
python3 tests/test-package-internal.py
python3 tests/test-entrypoints.py
python3 tests/test-update-key.py
python3 -O tests/test-update-key.py
python3 tests/test-apply-update.py
python3 -O tests/test-apply-update.py
python3 tests/test-external-activation.py
python3 tests/test-external-scp-init.py
python3 tests/test-image-cleanup.py
python3 tests/test-update-bundle.py
python3 -O tests/test-update-bundle.py
python3 tests/test-package-rear-brightness.py
python3 -O tests/test-package-rear-brightness.py
python3 tests/test-update-complete-tuning.py
python3 -O tests/test-update-complete-tuning.py
python3 tests/test-build-modules-from-tree.py
python3 -O tests/test-build-modules-from-tree.py
python3 tests/test-external-usb-guard.py
python3 -O tests/test-external-usb-guard.py
python3 tests/test-compressed-firmware.py /compiled/stock.dtb
python3 tests/test-link-state.py
python3 tests/test-gst-state.py
python3 tests/test-handoff-wait.py
python3 tests/test-package-paths.py
python3 -O tests/test-package-paths.py
python3 modules/mt8183-p1-public/tests/run_stop_gate.py
python3 modules/ov02a10-standard-fps-range/tests/run.py --no-save
python3 packaging/postmarketos/test-dtb-integration.py /compiled/stock.dtb /local/active.dtb
python3 -O packaging/postmarketos/test-dtb-integration.py /compiled/stock.dtb /local/active.dtb
python3 tests/test-release-safety.py /compiled/stock.dtb
```

Harnesses extract actual production functions and use thin framework/provider shims. PipeWire/GStreamer retain original MIT input headers; driver/DT tests declare GPL-2.0-only. Production validation and DT test oracles work under optimization. The release-safety test uses synthetic Image bytes solely to test SHA256/input/output contracts, not actual kernel acceptance. Full application, concurrency and hardware tests remain separate.

The handoff test checks actual idle-wait/STOP/active-deadline functions in seven ASan/UBSan cases, including first publication after 2000 ms and 4350 ms. [Hardware evidence](../docs/LOW_FPS_FIX.md) is separate.

The self-contained package-path test checks preserved-sidecar validation, a fixed P1 destination, symlink/hardlink rejection and use of the captured P1 bytes. Run it with normal Python and `-O` as shown above. It uses temporary fixtures and mocked packaging commands; it does not require private keys, a native APK toolchain or a device. [S2 validation](../docs/S2_VALIDATION.json) records the helper checks separately from S1 hardware acceptance.

The new-kernel build-helper tests use substitute Kbuild/compiler commands to check input rejection, all five module operations, cleaning only copied outputs, and preservation of existing output. They do not compile ARM64 modules or establish kernel compatibility.

The external guard tests use redirected sysfs/device fixtures and substitute blockdev/blkid tools: no real devices are accessed. Compressed-firmware tests run the actual checker with raw and zstd fixtures under normal Python and optimization, including malformed/missing data and decoder failure. They do not establish USB boot acceptance.

The user entry-point, key policy, image cleanup and bundle tests use temporary fixtures and simulated commands. They never invoke real sudo, APK installation, mounts or a device. The current SENINF format test is `python3 modules/mt8183-seninf-dual-highres/tests/run_formats.py`; `modules/mt8183-seninf-dual-highres/test-state.py` is an obsolete historical harness and is excluded from the current suite. It requires fixture updates before reuse.

The updater tests cover configuration edits, creation, deletion, permission/ownership and link changes, refusal before package/world/session writes, ordinary restoration, known interrupted states, atomic replacement failure, old backups lacking expectations and APK config prediction with preserved original files. Boot tests cover remount failure after `latest` persistence, cancellation after interruption, retry admission, retained backups and refusal when raw data, targets, identity, packages, backups or write journals differ. The boot wrapper shares the low-level transaction lock so cancellation cannot race application. These tests do not establish hardware acceptance of the recovery changes.

Rear-brightness distribution tests cover constrained repackaging, preservation of non-tuning payloads and attributes, index correspondence and refusal of existing output. Complete-source tests cover the one-line tuning change, member/internal-checksum preservation and rejection of unsafe paths or unexpected changes. Bundle tests connect all three source tuning files to the IPA payload manifest and complete source. Host APK fixtures, native signature/re-extraction checks and hardware capture are separate evidence.
