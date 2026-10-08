# License guide

English | [日本語](LICENSE.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->

This is a collection of separately licensed components. This guide does not replace the original licenses or apply one license to the whole tree.

| Component | Governing license / notice |
| --- | --- |
| P1 and sensor/receiver/lens drivers | Original per-file GPL tags; project P1 additions GPL-2.0-only; incorporated RAW reference retains its MIT notice |
| libcamera core / SoftISP | LGPL-2.1-or-later, plus original per-file licenses; RPi AF BSD-2-Clause |
| cam tool | GPL-2.0-or-later |
| PipeWire / built SPA / GStreamer plugin | MIT and retained upstream notices; full upstream source also contains separately licensed files |
| Optional official Alpine zstd 1.5.7-r2 | BSD-3-Clause choice for dual-licensed Zstandard code; separate BSD-2-Clause zstdgrep and MIT divsufsort notices retained; [fixed source and notices](developer/docs/OFFICIAL_ZSTD.json) |
| Tuning | CC0-1.0 |
| Original public packaging / integration scripts | Individual SPDX declarations, mainly MIT |
| New public guides and Japanese translations | CC0-1.0; third-party quotations and license texts keep their original terms |

License texts are in [LICENSES](developer/LICENSES/), component directories and the complete upstream source archive. Actual attribution is retained in source headers, [LIBCAMERA-COPYING.rst](developer/LIBCAMERA-COPYING.rst), [PIPEWIRE-COPYING](developer/PIPEWIRE-COPYING), and the installed APK NOTICE/COPYING files. The RAW MIT notice is in [the P1 license directory](developer/modules/mt8183-p1-public/LICENSES/softisp-MIT.txt).

Read [PROVENANCE](developer/docs/PROVENANCE.md) and [OSS release materials](developer/docs/OSS.md) for fixed sources, binary/source correspondence and publication scope. Publish the current binary archive together with the source kit and complete-source archive. No private key, device firmware or kernel binary is licensed for redistribution by this guide.

Japanese pages explain the same project information. They do not replace or translate the governing English license texts. Follow each component's original license, SPDX headers and upstream metadata.

[Project-owned MIT text](developer/LICENSES/PROJECT-MIT.txt) applies only to project-owned files already declared MIT; it does not replace upstream MIT notices. See [NOTICE](NOTICE.md).

Project host regressions and export auditors carry explicit per-file SPDX declarations: driver/DT harnesses are GPL-2.0-only and PipeWire/GStreamer wrappers are MIT. Historical OV02A10 fixtures retain their original GPL/MediaTek notices and provenance in tests/fixtures/sources.json.
