# DW9768 lens driver

The DW9768 VCM driver is based on Linux 6.18.28 with the original MediaTek 2020 copyright and GPL-2.0 license. Target: 6.18.28-mt81 ARM64/KCFI. Board wiring follows ChromiumOS commit `527db0b5974bb70364fc692448a55fb37209fe23`: I2C2 address 0x0c, vin 1.8 V and vdd 2.8 V, associated with the OV8856 lens-focus phandle.

The standard FOCUS_ABSOLUTE control exposes 0–1023. Driver build, lens recognition and fully calibrated autofocus are separate results; complete AF calibration remains unfinished. The project libcamera controller changes are described in the developer guide.

[Build guide](../../docs/BUILD.md) / [versioned hardware results and limits](../../docs/TESTING.md).
