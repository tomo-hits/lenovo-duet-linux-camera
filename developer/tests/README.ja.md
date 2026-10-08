# host回帰試験

[English](README.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->

source-kitのrootからPython・Clang/cc・ASan/UBSanを使って実行します。DTの3コマンドには、コンパイルしたSKU176 stock DTと、指定されている場合はローカルactive DTが必要です。実機には接続しません。OV02A10回帰試験は同梱する過去のGPL比較fixtureと出典を使います。

```sh
cd /absolute/path/to/lenovo-duet-linux-camera/developer
python3 tests/test-boot-update.py
python3 -O tests/test-boot-update.py
python3 tests/test-probe-idle.py
python3 tests/test-update-storage.py
python3 tests/test-package-internal.py
python3 tests/test-entrypoints.py
python3 tests/test-update-key.py
python3 -O tests/test-update-key.py
python3 tests/test-apply-update.py
python3 -O tests/test-apply-update.py
python3 tests/test-external-activation.py
python3 tests/test-external-scp-init.py
python3 tests/test-image-cleanup.py
python3 tests/test-update-bundle.py
python3 -O tests/test-update-bundle.py
python3 tests/test-package-rear-brightness.py
python3 -O tests/test-package-rear-brightness.py
python3 tests/test-update-complete-tuning.py
python3 -O tests/test-update-complete-tuning.py
python3 tests/test-build-modules-from-tree.py
python3 -O tests/test-build-modules-from-tree.py
python3 tests/test-external-usb-guard.py
python3 -O tests/test-external-usb-guard.py
python3 tests/test-compressed-firmware.py /compiled/stock.dtb
python3 tests/test-link-state.py
python3 tests/test-gst-state.py
python3 tests/test-handoff-wait.py
python3 tests/test-package-paths.py
python3 -O tests/test-package-paths.py
python3 modules/mt8183-p1-public/tests/run_stop_gate.py
python3 modules/ov02a10-standard-fps-range/tests/run.py --no-save
python3 packaging/postmarketos/test-dtb-integration.py /compiled/stock.dtb /local/active.dtb
python3 -O packaging/postmarketos/test-dtb-integration.py /compiled/stock.dtb /local/active.dtb
python3 tests/test-release-safety.py /compiled/stock.dtb
```

ハーネスは実際のproduction関数を抽出し、薄いframework/provider shimで検査します。PipeWire/GStreamer入力の原MIT表示を保持し、driver/DT試験にはGPL-2.0-onlyを明記します。productionの検証とDT試験の判定は最適化でも働きます。release-safetyのImageはSHA256・入力／出力契約を検査する合成データで、実kernel受入ではありません。アプリ・並行処理・実機の確認とは区別します。

handoff試験は実際の待機/STOP/実処理期限の関数をASan/UBSanで7cases検査し、2000ms・4350ms後の初回publishを含みます。[実機結果](../docs/LOW_FPS_FIX.ja.md)は別に記録しています。

自己完結したpackage-path試験は、保全sidecarの検証、P1書込先の固定、symlink/hardlink拒否、読み取り済みP1 bytesの使用を検査します。上記の通常Pythonと `-O` の両方で実行してください。一時fixtureと代替の梱包commandを使うため、秘密鍵・native APK toolchain・実機は不要です。[S2検証](../docs/S2_VALIDATION.json)ではhelperの確認をS1実機受入と分けて記録します。

新しいカーネル向けビルド補助スクリプトの試験は、代替のKbuild・コンパイラーで入力拒否、5モジュールの処理、コピー先だけのclean、既存出力の保護を検査します。実際のARM64コンパイルやカーネル互換性の試験ではありません。

外部起動保護の試験は隔離sysfs/device fixtureと代替blockdev/blkidを使い、実デバイスへアクセスしません。圧縮firmware試験はraw/zstd fixtureで実checkerを通常/-O実行し、破損・欠落・decoder失敗を確認します。USB実起動の確認とは別です。

利用者向けシェル、署名鍵、イメージ後片付け、配布梱包の試験は、一時ファイルと模擬コマンドを使います。実sudo、APK導入、mount、実機操作は行いません。現行SENINFの形式試験は `python3 modules/mt8183-seninf-dual-highres/tests/run_formats.py` です。`modules/mt8183-seninf-dual-highres/test-state.py` は旧fixtureのまま保存した歴史的な試験で、現行試験の対象外です。再利用にはfixtureの更新が必要です。

更新処理の試験は設定の編集・作成・削除・権限／所有者・リンク変更、パッケージ／world／sessionの変更前の拒否、通常復元、既知の途中状態、atomic置換失敗、照合情報のない旧バックアップ、元の設定を保持したAPK設定の予測を検査します。起動処理の試験はlatest保存後のremount失敗、中断後の取消、再試行の受付、バックアップ保持と、起動領域・保存対象・媒体識別・パッケージ・バックアップ・書込みjournalが異なる場合の拒否を検査します。起動wrapperは低水準transactionとlockを共有し、取消と適用の競合を防ぎます。これらは復旧修正の実機受入を示すものではありません。

後の明るさ配布試験は、元の署名パッケージの限定再梱包、設定以外のpayloadと属性の保持、署名indexの対応、既存出力の拒否を検査します。完全ソースの試験は1行のtuning変更、member・内側checksumの保持、危険なpathや差分の拒否を検査します。bundle試験はソース・IPA payload manifest・完全ソース内の3つのtuningの対応を検査します。ホストの模擬APK試験と、nativeでの実署名・再展開検証、実機撮影は別の確認です。
