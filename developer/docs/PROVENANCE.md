# Provenance and licensing

English | [日本語](PROVENANCE.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->

| Component | Fixed source | License |
| --- | --- | --- |
| P1 / SENINF firmware ABI and receiver | ChromiumOS chromeos-5.10 `527db0b5974bb70364fc692448a55fb37209fe23`; per-file references in modules | GPL-2.0; project P1 additions GPL-2.0-only |
| OV02A10 / OV8856 / DW9768 | Linux 6.18.28, with the included sensor timing/control changes | GPL-2.0, original MediaTek/Intel authors retained |
| Linux build input | kernel.org Linux 6.18.28 tarball + seven included postmarketOS patches | GPL-2.0-only and original per-file licenses |
| libcamera core | `c0049ea0605c1492c99b8f82bb04661a04cf1bf1` (0.7.2) + complete patch | LGPL-2.1-or-later |
| cam | Same libcamera source | GPL-2.0-or-later |
| RPi AF controller | Existing libcamera RPi controller, used by the complete patch | BSD-2-Clause |
| Tuning | Included YAML files | CC0-1.0 |
| PipeWire / SPA / GStreamer plugin | 1.6.8 + five ordered patches | MIT |
| Optional official Alpine zstd | Unmodified Alpine v3.24 main/aarch64 zstd 1.5.7-r2; upstream v1.5.7 and aports recipe `3c6e2ee2b16f403d53eab39c4426eb61f003c322`; hashes and source URLs in [OFFICIAL_ZSTD](OFFICIAL_ZSTD.json) | BSD-3-Clause choice; separate zstdgrep BSD-2-Clause and divsufsort MIT notices retained |
| RAW packing reference | Source attribution in camera_raw.c | MIT; modules/mt8183-p1-public/LICENSES/softisp-MIT.txt |
| Packaging, public boot helpers and host regressions | Included sources, individual SPDX headers | MIT or GPL-2.0-only; historical fixtures retain original GPL |
| New public guides and Japanese translations | README and public docs | CC0-1.0; third-party quotations and license texts retain their original terms |

The pmaports checkout used for the input recipe was `368093c7a882637ee00d32932fb0dafd24cfc4d4`. Included APKBUILD, seven patches and configurations are the authoritative content, with file hashes in SHA256SUMS; the prepared matched KCFI configuration is supplied separately as `configs/kernel/matched.config`. BUILD records how it is used without copying the old SDK.

Original module `sources.json`, source comments, LICENSES, LIBCAMERA-COPYING.rst and PIPEWIRE-COPYING remain part of the source distribution. Historical source metadata describes its original experiment; current acceptance is in TESTING. No single top-level license overrides these component licenses.

SCP firmware and the matching kernel Image are user-provided inputs and are not release assets. Camera images and unselected development logs are not published.

Fresh APKs also include component NOTICE files generated from the fixed upstream/kernel source copyright headers, the actual PipeWire COPYING and libcamera COPYING.rst, and the P1 RAW packing MIT notice. Standard license templates do not replace these actual notices.

For complete-source distribution and current versus historical publication scope, see [OSS](OSS.md). The original license texts remain authoritative; Japanese pages are explanatory project documentation.
