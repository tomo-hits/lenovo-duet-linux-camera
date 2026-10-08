# Historical MT8183 P1 interface cleanup

This historical GPL-2.0-only implementation targets 6.18.28-mt81 ARM64/KCFI and krane SKU176. Media graph and firmware ABI references derive from MediaTek 2019 GPL-2.0 ChromiumOS commit `527db0b5974bb70364fc692448a55fb37209fe23`; project additions retain GPL-2.0-only.

V4L2/VB2/Media Controller/runtime PM replace development sysfs controls and fault-injection parameters. Optional root-only numeric debugfs diagnostics expose no images or stream controls. Reader teardown precedes state removal; absent debugfs does not prevent ordinary apps.

STREAMOFF checks input/IRQ/SCP/IPI/CAM stop and ownership. Normal close retains stopped DMA for reuse; actual release occurs during retirement/removal after joined workers and positive ownership proof. Failure retains DMA and blocks reuse. Active-camera sleep and forced provider teardown are unsupported.

`python3 tests/run_stop_gate.py` extracts real functions for 15 synthetic ASan/UBSan cases; it does not prove hardware concurrency. Historical native output SHA256: `e8efe06add92a5e32aa4388aa531cc4dbe164d9b1efae880826744540f2c291e`. The selected distributable P1 is the sibling `mt8183-p1-public` implementation.

[Build guide](../../docs/BUILD.md) / [versioned hardware results and limits](../../docs/TESTING.md).
