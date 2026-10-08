# ライセンス案内

[English](LICENSE.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->

部品ごとに異なるライセンスを持つソースの集合です。この案内は原licenseを置き換えたり、全体に単一licenseを適用したりしません。

| 部品 | 適用するライセンス／表示 |
| --- | --- |
| P1・sensor/receiver/lens driver | 各fileの原GPL表示、独自P1追加GPL-2.0-only、RAW参照の原MIT noticeも保持 |
| libcamera core/SoftISP | LGPL-2.1-or-laterと各fileの原条件、RPi AFはBSD-2-Clause |
| cam | GPL-2.0-or-later |
| PipeWire・ビルド対象SPA・GStreamer plugin | MITと原著作権表示、完全upstream source内には別条件のfileもある |
| 補助依存の公式Alpine zstd 1.5.7-r2 | Zstandardの二重licenseからBSD-3-Clauseを選択。別条件のzstdgrep BSD-2-Clauseとdivsufsort MIT表示も保持。[固定出典と原表示](developer/docs/OFFICIAL_ZSTD.json) |
| tuning | CC0-1.0 |
| 公開packaging/統合script | 各SPDX、主にMIT |
| 新規公開ガイドと日本語訳 | CC0-1.0。第三者の引用／license本文は原条件を保持 |

license本文は[LICENSES](developer/LICENSES/)、各component directory、完全upstream source archiveにあります。実著作権表示はsource header、[LIBCAMERA-COPYING.rst](developer/LIBCAMERA-COPYING.rst)、[PIPEWIRE-COPYING](developer/PIPEWIRE-COPYING)、APK内のNOTICE/COPYINGへ保持します。RAWのMIT本文は[P1 LICENSES](developer/modules/mt8183-p1-public/LICENSES/softisp-MIT.txt)にあります。

固定出典・binary/source対応・公開範囲は[PROVENANCE](developer/docs/PROVENANCE.ja.md)と[OSS公開資料](developer/docs/OSS.ja.md)を参照してください。現行APKとsource kitと完全source archiveを同じ公開場所へ添えます。この案内は秘密鍵・firmware・kernel binaryの再配布許諾を与えません。

日本語文書はプロジェクト情報の説明です。正式な英語license本文の置き換えや翻訳licenseではありません。各部品の原license、SPDX header、upstream metadataに従ってください。

[プロジェクト所有MIT本文](developer/LICENSES/PROJECT-MIT.txt)は既にMITと宣言したプロジェクト所有fileだけに対応し、upstreamのMIT著作権表示を置き換えません。[NOTICE](NOTICE.md)も参照してください。

独自host回帰試験とexport監査にはfile別SPDXを明記しています。driver/DTハーネスはGPL-2.0-only、PipeWire/GStreamer wrapperはMITです。OV02A10の歴史的fixtureは原GPL・MediaTek表示とtests/fixtures/sources.jsonの出典を保持します。
