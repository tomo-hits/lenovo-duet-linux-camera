# Historical P1 frame timing and sleep implementation

This GPL-2.0-only implementation targets Linux 6.18.28-mt81 ARM64/KCFI and the inherited MT8183 firmware ABI. It is retained for comparison/regression testing; the selected distributable implementation is `mt8183-p1-public`.

`capture_conditions` reads the active V4L2 VBLANK minimum/maximum instead of a fixed lower bound. It does not write sensor controls; pattern and stopped-input guards remain. `tests/run_capture_conditions.py` extracts the actual function and checks valid/missing/invalid ranges under ASan/UBSan. Synthetic controls do not prove hardware behavior.

Media Controller selects one active front/rear input. Associated sensor/SENINF modules support front 1600×1200 and rear 1632×1224 / 3264×2448. `PM_SUSPEND_PREPARE` retires normally stopped sessions before runtime PM is disabled, with ownership checks; `PM_POST_SUSPEND` permits a new start. Active or failed sessions reject sleep.

Historical rear packed RAW10 uses stride 2040, image size 2496960 and page stride 2498560; unpacked RAW10 uses stride 3264 and image size 3995136. Legacy camera_softisp is not built by the Makefile. Older raw-interface fixtures require geometry updates; their failure does not establish a camera failure or a passing full suite.

[Build guide](../../docs/BUILD.md) / [versioned hardware results and limits](../../docs/TESTING.md).
