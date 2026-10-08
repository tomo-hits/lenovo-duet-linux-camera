# 試験と検証範囲

[English](TESTING.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->


ビルド成功、センサー認識、画像取得、画質評価、復元は別の到達点です。各記録は記した版・環境に限定します。ホストfixtureはソフトウェアの挙動を検査するもので、実機互換性や並行動作の証明ではありません。

## v1.0.1と後gamma比較（2026-10-08）

[ACCEPTANCE_2026-10-08.json](ACCEPTANCE_2026-10-08.json)は次を分けています。

- **v1.0.1：** 起動準備、導入・再起動、Snapshotで前後各15分撮影、スピーカー聴取、カメラを閉じた28.4427秒のdeep sleepと復帰後撮影、設定外部変更の拒否、未書込みPREPAREDの取消、通常復元と再起動・ログイン後の記録対象照合が成功しました。マイク録音や全filesystem一致は未評価です。
- **後gamma：** 同じv1.0.1 ELFと標準Gamma controlによる7ケース／2,880 requestで、30→15→30 fps、gamma 2.4→2.2、前後切替を確認しました。全件STOP正常・SCP offlineで、後の受信buffer時刻から約30.01155 fpsでした。別工程の同じ場面のSnapshot比較では、実候補YAMLを使い後の中間調の視認性改善と前の大きな異常なしを確認しました。元OSへ復元し、再起動後に記録対象を照合しました。

7ケースのmetadata試験は候補YAMLを読み込まず、目視試験では読み込んでいます。受信buffer時刻とdriver完了counterはSnapshot表示fpsではありません。初回tuning変更の31ホスト試験は通常Python／`-O`計62回成功しました。native署名・payloadとsourceの一致・梱包は別の[REAR_BRIGHTNESS_PACKAGING](REAR_BRIGHTNESS_PACKAGING.json)に記録しています。**libcamera r103／meta 0.2.13の新APKセットの導入・復元は未試験です。**

## 過去版の実機記録

| 版・環境 | 結果と制限 | 記録 |
| --- | --- | --- |
| v1.0.0、内蔵eMMC | 前後各3,600frames、平均取得約24 fps、20 JPEG、再利用、限定したsleep・復旧。画質・音声の制限は別記 | [PUBLIC_ACCEPTANCE](PUBLIC_ACCEPTANCE.json) |
| Modules 0.2.6／meta 0.2.10、内蔵eMMC | 短時間Snapshot撮影・新process再利用・記録対象復元。bootは別工程。実測fps・耐久・音声・sleepは未受入 | [INTERNAL_ACCEPTANCE](INTERNAL_ACCEPTANCE.json) |
| Modules 0.2.5／meta 0.2.9、外部USB | canonical DT cold boot、自動有効化、撮影・再利用、元838packages・world・設定へ復元。復元後USB起動と音声聴取は未確認 | [USB_ACCEPTANCE](USB_ACCEPTANCE.json) / [UPDATE_VALIDATION](UPDATE_VALIDATION.json) |
| S1／v6、RAM | timing・再利用6session／194frames、初回STOP 0.0645秒と再利用、Snapshot別process2回・5 JPEG。新boot・耐久・校正は未評価 | [S1_VALIDATION](S1_VALIDATION.json) / [LOW_FPS_FIX](LOW_FPS_FIX.ja.md) |
| Fresh v4以前、RAM | 5module build、固定SDK・export検査、通常アプリ撮影、署名セットの切戻し・再適用。長時間・高解像度・sleepは各版に限定 | [BUILD_RECORD](BUILD_RECORD.json) / [REAR-FPS](../REAR-FPS.md) / [共通stack修正](../UPSTREAM-STATE-FIXES.md) |

## 失敗条件と回帰試験

初期USB更新はmodules 0.2.4-r1を導入しましたが、再起動後にP1のRAM専用検査でloadを拒否しました。元パッケージ・world・設定の復元が成功し、その後の保護検査付きUSB対応でloadを解消しました。別の初回撮影はSCP recoveryがenabledのためEPERM、firmware command・frameとも0でした。起動helperがdisabledの読戻しを検証する修正後、撮影・再利用が成功しました。[P1_USB_FIX](P1_USB_FIX.json)、[EXTERNAL_PREPARATION](EXTERNAL_PREPARATION.json)、[SCP_RECOVERY_FIX](SCP_RECOVERY_FIX.json)。

[S1](LOW_FPS_FIX.ja.md)はcapture進捗とSTOPの期限を維持して低fps時のidle publisher timeoutを修正します。[S2](PUBLICATION_FIXES.ja.md)は保全入力のsidecarを検証し、P1の書込先を固定します。[ホスト試験ガイド](../tests/README.ja.md)にコマンドとfixtureの限界を記します。DT試験は別途コンパイルしたstock/active DTが必要で、合成boot Imageは実起動の証拠ではありません。

## 実機確認の手順

アプリを通常終了し、STOP/idleを確認して完全な対応署名セットと対応する更新・有効化手順を使います。`cam -l`の2sensor、前後live・保存・切替・通常終了・新process再利用を確認し、error・frame進行・exit・package/module対応を記録します。通常音声、アプリ閉鎖後sleep、復元は別に確認します。強制unloadや強制rebootを通常回収の成功として扱いません。

完全な色・ノイズ・flicker・距離別AF校正、日単位耐久、他SKU・kernel、codec/SCP共存は未確認です。[既知の制限](../../docs/KNOWN_ISSUES.ja.md)。
