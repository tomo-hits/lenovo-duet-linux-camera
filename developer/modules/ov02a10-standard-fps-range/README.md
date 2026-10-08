# OV02A10 standard frame-rate controls

The front sensor code derives from Linux 6.18.28, retaining MediaTek 2020 copyright and GPL-2.0. Target: 6.18.28-mt81 ARM64/KCFI. The 1600×1200 mode register table/PLL remain unchanged. VBLANK minimum is 190 (VTS 1390); default 1580 is retained. Standard FrameDurationLimits can request 20/25/30 fps.

RAW colour space, no transfer function and full range are returned explicitly. UNIT_CELL_SIZE is read-only at 1750 nm, based on the recorded MCNEX module specification. Physical location follows DT orientation. Paced I2C, cold startup, flip controls, diagnostics, exposure/gain and STOP behavior are retained.

`python3 tests/run.py --no-save` checks actual control/cold/live/fault paths using the bundled GPL fixtures. Historical Snapshot 1536×864 runs passed 20/25 fps for 60 seconds and 30 fps for 300 seconds with saved images and normal close. These are version-specific throughput checks, not full quality calibration.

[Build guide](../../docs/BUILD.md) / [versioned hardware results and limits](../../docs/TESTING.md).
