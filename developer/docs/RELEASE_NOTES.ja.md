# 版ごとの変更記録

[English](RELEASE_NOTES.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->


各ソフトウェア版の変更と検証を記します。取得できる配布物は[Releases](https://github.com/tomo-hits/lenovo-duet-linux-camera/releases)で確認してください。ここに過去版が記載されていても現在の配布を保証しません。

## 後の明るさ調整：v1.0.2パッケージ対応ソース（2026-10-08）

後OV8856 Adjustへgamma 2.4を追加して中間調を明るくします。前tuning・AE・contrast既定1.0・30 fps要求は維持します。libcamera4APKは`0.7.2-r103`、meta2APKは`0.2.13-r0/r1`で、他5APKと全63 ELFはv1.0.1と一致します。完全ソースのtuning、署名index対応、payload属性を検査しています。[梱包記録](REAR_BRIGHTNESS_PACKAGING.json)。

同じバイナリとGamma controlのmetadata7ケース／2,880 requestはSTOP正常・SCP offlineで完了しました。別工程の同じ場面の候補YAML比較では後の視認性改善と前の大きな異常なしを確認しました。Snapshot表示fpsや全面校正の証明ではありません。**新APKセットの導入・復元は未試験です。**[実機記録](ACCEPTANCE_2026-10-08.json)。

## 復旧保護：v1.0.1（2026-10-07）

設定復元は内容・権限・所有者・リンク・不在とapk worldを全書込み前に照合します。中断時は記録したbefore/package/runtime/restore状態だけを許可します。未書込みPREPAREDは媒体・元領域・保存対象・パッケージ・backupを照合して取消・再試行でき、backupを保持します。

camera11APK・公開鍵・metadata・完全sourceはv1.0.0とbyte同一です（modules 0.2.8-r0／meta 0.2.12-r1）。この版のホスト試験はPython各mode133件でした。別の2026-10-08実機試験で導入・撮影・音声・sleep・復元を確認しています。[TESTING](TESTING.ja.md)。

v1.0.0のpackage backupには設定の期待状態がなく、新スクリプトは自動復元を拒否します。元kit・backupを保管して個別確認で復旧し、旧transactionへの重ねた導入や設定編集後の旧復元処理は行わないでください。

## 内蔵導入の統合：v1.0.0（2026-10-07）

対応する内蔵eMMCの起動準備、署名package導入、記録対象復元を追加しました。Modules 0.2.8-r0／meta 0.2.12-r1で、前後撮影・再利用・限定sleep・復元が成功しました。30 fps要求に対する平均取得は約24 fpsでした。[PUBLIC_ACCEPTANCE](PUBLIC_ACCEPTANCE.json)に当時の正確な制限を記します。

## 内蔵対応の開発：modules 0.2.6／meta 0.2.10（2026-10-06）

bootを別途準備した環境向けの明示内蔵modeを追加しました。短時間アプリ撮影・再利用・STOP正常・package復元と別工程のboot・設定復元が成功しました。当時は起動準備の統合ツールがありません。[INTERNAL_ACCEPTANCE](INTERNAL_ACCEPTANCE.json)。

## 外部USB対応の開発：modules 0.2.5／meta 0.2.9（2026-10-06）

保護検査付きUSB P1有効化とSCP recovery初期化を修正しました。canonical DT cold boot、自動有効化、撮影・再利用、元package・world・設定復元が成功しました。復元後USB起動と音声聴取は未確認です。[USB_ACCEPTANCE](USB_ACCEPTANCE.json)。

## 以前のbuild・梱包（2026-10-05〜06）

- 外部準備では通常ファイルimageへのboot統合、UUID/eMMC initramfs保護、直接password設定を追加し、配布元所有のPulse moduleを除いて競合を避けました。[EXTERNAL_PREPARATION](EXTERNAL_PREPARATION.json)。
- S1は低fpsのidle publisher待機を修正し、限定したRAM撮影・STOP・再利用が成功しました。[LOW_FPS_FIX](LOW_FPS_FIX.ja.md)。
- S2は保全入力の全sidecarを検証し、P1置換先を固定します。[PUBLICATION_FIXES](PUBLICATION_FIXES.ja.md)。
- source build・新kernel helper、shell入口、完全ソース、部品noticeは[開発者ガイド](../README.ja.md)に記します。build helperのホスト試験で新kernel対応を実証したものではありません。

部品の原licenseを維持します。配布条件は[OSS](OSS.ja.md)、制限は[KNOWN_ISSUES](../../docs/KNOWN_ISSUES.ja.md)を参照してください。
