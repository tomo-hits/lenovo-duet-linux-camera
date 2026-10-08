# OV8856 balanced readout timing

This experimental sensor timing derives from Linux 6.18.28, retaining Intel 2019 copyright and GPL-2.0. Target: 6.18.28-mt81 ARM64/KCFI. Original 1632×1224 four-lane Bayer geometry, PLL/MIPI and full 3264×2448 values are retained. The binned timing uses HTS 3820, VTS minimum 1256 and default 2512.

These timing values occur in the upstream neighboring 1640×1232 mode; their combination with 1632×1224 is experimental and not upstream-validated. Readout is about 32.47 ms, minimum frame duration 33.319 ms and default 66.638 ms. Standard HBLANK/VBLANK/exposure/duration conversions use actual line timing. P1 ownership guards remain unchanged; there is no private AE/AWB algorithm.

An earlier native-timing 20 fps experiment failed when copy completion crossed DONE by 89 µs. The balanced timing's register/state/full-resolution regressions and later RAM application checks are recorded in [REAR-FPS](../../REAR-FPS.md). Host helpers do not establish hardware safety or fully calibrated image quality.

[Build guide](../../docs/BUILD.md) / [versioned hardware results and limits](../../docs/TESTING.md).
