# 検証と梱包の保護

[English](PUBLICATION_FIXES.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->


S1前v5の検証helper修正と、後のS2梱包検査を説明します。結果は記録した版に限定し、その後の実機結果は[TESTING](TESTING.ja.md)に記します。

## 検証helperの修正

DT適合、provider/export、入力hashの検査を、最適化Pythonでも働く明示的な例外にしました。DT出力は既存file・symlinkを拒否し、boot assetは検証済み入力だけをコピーします。boot helperの既定ABI JSONは隣接fileです。

独立auditorとhost harnessは原sourceに適合するSPDXと第三者noticeを保持します。camera5APKを実PROJECT-MIT本文で再発行し、libcamera/PipeWire APK・camera ELF・設定byteを保持しました。[PACKAGE_FIXES](PACKAGE_FIXES.json)に変更file、[PUBLICATION_REVIEW](PUBLICATION_REVIEW.json)にhost/native検査を記します。このhelper・notice修正ではcamera実機の追加受入を行っていません。OV02A10比較fixtureはGPL source・出典を含み、外部の開発ディレクトリに依存しません。

## S2：保全入力の検証と固定した置換先

APK署名の確認だけでは隣の`payload-hashes.json`を検証できず、そのpathがP1の書込先に影響しました。helperは出力前にpackage集合・正規path・SHA256形式・重複keyを含む全sidecarを検証します。保全sidecar hashは`34efbd109b4b2d3ce72bc327fe3972e35cf23907fe84aa67e32e8e59b9eab88e`です。

P1置換はpackage内の固定pathを使い、途中・末尾symlinkと書込先hardlinkを拒否します。hash・ELF/modinfo・梱包は同じ読み取り済みbyteを使います。`tests/test-package-paths.py`を通常Python／`-O`で実行し、[S2_VALIDATION](S2_VALIDATION.json)にfixtureと別のnative梱包検査を記します。v6 APKとS1 runtime codeは不変で、新しい実機受入ではありません。
