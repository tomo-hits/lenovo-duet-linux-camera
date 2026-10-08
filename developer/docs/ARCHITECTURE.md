# Architecture

English | [日本語](ARCHITECTURE.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->

The current composition is OV02A10 (front), OV8856 (rear), DW9768 lens, MediaTek SENINF receiver and MT8183 P1 RAW capture. Standard sensor/lens controls feed the media graph. P1 exposes V4L2/VB2 buffers and standard stream operations. RAW10 conversion and copying remain CPU work; libcamera Simple/SoftISP supplies shared AE/AWB/debayer algorithms and the existing RPiController AF core. PipeWire's libcamera SPA node reaches standard desktop applications through the portal. Direct libcamera tools can select rear high resolution.

P1/SENINF retain the fixed MediaTek firmware/register protocol derived from ChromiumOS `527db0b5974bb70364fc692448a55fb37209fe23`. This is not a replacement for the full ISP. Codec/SCP concurrent ownership has not been accepted.

Normal STREAMOFF requires input stop, IRQ synchronization, SCP/IPI shutdown, positive CAM power/ownership proof and worker finalization. Failed proof retains DMA and blocks reuse. Successful normal Close retains stopped DMA for rearm; later retirement/device removal performs release after ownership proof. Active-camera sleep is unsupported; close the application first.

There is no private sysfs image/control interface or fault-injection parameter in the default P1. Optional root-only numeric debugfs diagnostics live under `/sys/kernel/debug/mt8183_p1/<device>/`; applications work without debugfs.

`mt8183_p1_legacy.h` isolates accepted legacy DT/boot identifiers and conflicting-driver exclusions. The canonical DT producer validates the whole graph, resolves actual provider phandles and preserves existing properties/reservations. Its output is a local proposal, not an approved upstream binding. It never selects or writes a boot partition.
