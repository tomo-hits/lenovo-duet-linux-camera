# 対応ソースと著作権表示

[English](OSS.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->


## 現行ソースとパッケージの対応

[PUBLIC_PACKAGES](PUBLIC_PACKAGES.json)はmodules **0.2.8-r0**、config **0.1.3-r0/r1**、meta **0.2.13-r0/r1**、libcamera **0.7.2-r103**、PipeWire/GStreamer **1.6.8-r104**を記録します。[COMPLETE_SOURCES](COMPLETE_SOURCES.json)に`mt8183-camera-complete-sources-20261008-v1.tar.xz`とhashを記します。後gamma 2.4はソース・APK payload・完全ソースで一致します。nativeパッケージ検査は[REAR_BRIGHTNESS_PACKAGING](REAR_BRIGHTNESS_PACKAGING.json)です。梱包検証と実機導入の受入は別で、[TESTING](TESTING.ja.md)に確認範囲を記します。

## バイナリと一緒に提供する資料

署名APK・checksumとともに、対応するrepository source kitと完全ソースarchiveを提供します。kitは現行module全code、固定kernel設定・patch、userspace patch、build/install/package script、tuningを含みます。完全ソースはpatch適用済みlibcamera/PipeWire全入力、build script・原license、元Linux 6.18.28全sourceとtuning overrideを含みます。[PROVENANCE](PROVENANCE.ja.md)に出典と部品licenseを記します。異なるパッケージ版のkitで対応ソースを代用しないでください。

バイナリ提供中は対応ソースへ同等にアクセスできる状態を維持します。[GPLv2第3条](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html)、[LGPLv2.1第4条](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html)、[GNUのソース配布説明](https://www.gnu.org/licenses/gpl-faq.en.html#DistributingSourceIsInconvenient)を参照してください。個別copyright/SPDX、全原license、libcamera COPYING.rst、PipeWire COPYING、REUSE metadataを保持します。rootのlicenseガイドは部品licenseの置換ではありません。APKにも実著作権表示とP1 RAW packingのMIT本文を含みます。

[MODIFICATIONS](MODIFICATIONS.json)に変更codeと実build入力hashを記します。配布ソースの日付付き変更コメントは編集内容の表示であり、それだけで新build・試験を示しません。公開ガイドと訳はCC0-1.0とし、元の第三者資料は元条件を維持します。

## 公式zstd補助依存

内蔵OS用kitは、camera11APKとは別の`packages/official-zstd/`へ公式Alpine v3.24 `main/aarch64`の **zstd 1.5.7-r2** を無変更で含められます。既存の圧縮SCP firmwareを読み取り専用で検査するための展開処理です。未導入かつruntime依存が既存OSで満たされる場合だけ固定版を追加します。元署名とbyte列を保持します。

[OFFICIAL_ZSTD](OFFICIAL_ZSTD.json)にAPK/index hash、upstream [v1.5.7 source](https://github.com/facebook/zstd/archive/v1.5.7.tar.gz)、固定[Alpine recipe](https://github.com/alpinelinux/aports/blob/3c6e2ee2b16f403d53eab39c4426eb61f003c322/main/zstd/APKBUILD)を記します。原[LICENSE](../LICENSES/ZSTD-1.5.7-LICENSE.txt)、[COPYING](../LICENSES/ZSTD-1.5.7-COPYING.txt)、[source notice](../LICENSES/ZSTD-1.5.7-NOTICES.txt)を添えます。Zstandardの二重licenseからBSD-3-Clauseを選び、別条件のzstdgrep BSD-2-Clauseとdivsufsort MITも保持します。zstd sourceはカメラ完全ソースとは別資料です。

## 過去版の記録

[INTERNAL_PACKAGES](INTERNAL_PACKAGES.json)、[USB_PACKAGES](USB_PACKAGES.json)、[PACKAGES](PACKAGES.json)、[PRE_REAR_BRIGHTNESS_PACKAGES](PRE_REAR_BRIGHTNESS_PACKAGES.json)は各版の集合です。結果を別の版へ適用しません。旧バイナリの再配布には対応する旧sourceを使います。生成済み署名・IPA秘密鍵、実機firmware・kernel・OS画像、認証情報、カメラ画像は配布対象ではありません。
