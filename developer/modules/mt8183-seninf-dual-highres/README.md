# MediaTek SENINF receiver

This GPL-2.0-only receiver targets Linux 6.18.28-mt81 and the matched MT8183 camera graph. Fixed source provenance is recorded in [sources.json](sources.json). It uses the board's CSI/DPHY routing and forwards RAW data; it does not implement an ISP, AE or AWB.

The historical rear path uses four CSI0 lanes, RAW10 and sink0→source4. The current format regression is `python3 tests/run_formats.py`. `test-state.py` retains older fixtures and is not a current acceptance test without fixture updates. Build, recognition, acquisition and quality assessment are separate milestones.

[Build guide](../../docs/BUILD.md) / [versioned hardware results and limits](../../docs/TESTING.md).
