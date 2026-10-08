# MT8183 P1 RAW capture

The selected P1 driver targets 6.18.28-mt81 ARM64/KCFI, krane SKU176 and its matched MediaTek SCP firmware ABI. It exposes V4L2/VB2/Media Controller/runtime PM. ABI/register provenance is MediaTek 2019 GPL-2.0 ChromiumOS commit `527db0b5974bb70364fc692448a55fb37209fe23`; project additions are GPL-2.0-only. RAW packing retains its MIT reference and `LICENSES/softisp-MIT.txt`.

The local binding is `mediatek,mt8183-p1-raw` at `/camera@1a006000`; legacy DT/boot identifiers and competing-driver exclusions are isolated in `mt8183_p1_legacy.h`. The binding is not upstream-approved. DT integration validates identities, provider references and the complete graph. Other firmware/kernel variants are unsupported.

RAM is the guarded default. Internal-eMMC and external-USB modes are opt-in and must be activated through the matching helper, which checks media, kernel, DT and firmware. Do not supply mode parameters to bypass those checks. Current internal-mode build inputs and 203 audited exports are in [PUBLIC_P1_BUILD](../../docs/PUBLIC_P1_BUILD.json). Historical USB mode used modules 0.2.5 and 202 exports.

Optional root-only numeric diagnostics are under `/sys/kernel/debug/mt8183_p1/<platform-device>/`; applications do not require debugfs. Failed STOP retains DMA and prevents reuse. `python3 tests/run_stop_gate.py` exercises 15 extracted-function cases under ASan/UBSan using synthetic providers; hardware results are separate.

[Build guide](../../docs/BUILD.md) / [versioned hardware results and limits](../../docs/TESTING.md).
