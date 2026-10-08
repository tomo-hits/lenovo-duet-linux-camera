# 出典とライセンス

[English](PROVENANCE.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->

| 部品 | 固定出典 | ライセンス |
| --- | --- | --- |
| P1/SENINFのfirmware ABI・receiver | ChromiumOS chromeos-5.10 `527db0b5974bb70364fc692448a55fb37209fe23`、各fileの参照 | GPL-2.0、独自P1追加はGPL-2.0-only |
| OV02A10/OV8856/DW9768 | Linux6.18.28＋同梱timing/control変更 | 原GPL-2.0、MediaTek/Intel著作権表示を保持 |
| Linux build入力 | kernel.org6.18.28＋postmarketOS7patch | GPL-2.0-onlyと各fileの原license |
| libcamera core | `c0049ea0605c1492c99b8f82bb04661a04cf1bf1`（0.7.2）＋complete patch | LGPL-2.1-or-later |
| cam | 同じlibcamera source | GPL-2.0-or-later |
| RPi AF controller | 既存libcamera RPi controllerを利用 | BSD-2-Clause |
| tuning | 同梱YAML | CC0-1.0 |
| PipeWire/SPA/GStreamer plugin | 1.6.8＋順序付き5patch | MIT |
| 補助依存の公式Alpine zstd | Alpine v3.24 main/aarch64のzstd 1.5.7-r2を無変更で同梱。upstream v1.5.7、aports recipe `3c6e2ee2b16f403d53eab39c4426eb61f003c322`。hashとsource URLは[OFFICIAL_ZSTD](OFFICIAL_ZSTD.json) | BSD-3-Clauseを選択。別条件のzstdgrep BSD-2-Clauseとdivsufsort MITの表示も保持 |
| RAW packing参照 | camera_raw.c内の出典 | MIT、`modules/mt8183-p1-public/LICENSES/softisp-MIT.txt` |
| packaging・公開boot helper・host回帰試験 | 同梱sourceの個別SPDX | MITまたはGPL-2.0-only。歴史的fixtureの原GPLも保持 |
| このkitの新規公開ガイド・日本語訳 | READMEとdocs内の公開ガイド | CC0-1.0。引用した第三者文章・license本文はそれぞれの原条件 |

pmaports入力checkoutは `368093c7a882637ee00d32932fb0dafd24cfc4d4`。APKBUILD、7patch、設定はSHA256SUMSで固定し、準備済みKCFI設定は `configs/kernel/matched.config` です。旧SDKをコピーしない使い方を[BUILD](BUILD.ja.md)に記載します。

moduleのsources.json、source comment、LICENSES、LIBCAMERA-COPYING.rst、PIPEWIRE-COPYINGを保持します。古いmetadataは当時の実験を説明し、現在の受入はTESTINGが基準です。単一のroot licenseで部品の原licenseを上書きしません。正式なlicense条件は英語原文と各fileのSPDX／upstream metadataを参照してください。この日本語説明はlicense本文の置き換えではありません。

fresh APKには固定sourceから抽出した実copyright NOTICE、PipeWire COPYING、libcamera COPYING.rst、P1 RAW packingのMIT noticeも含みます。標準license templateだけで実著作権表示を代用しません。source kitと完全ソースarchiveをAPKと同じ公開場所へ置く方針・確認範囲は[OSS](OSS.ja.md)に記載します。

SCP firmware/kernel Imageは利用者側の入力で、配布しません。写真・未選別開発logsも公開しません。
