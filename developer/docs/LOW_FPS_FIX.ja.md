# 低fps時のpublisher待機修正（S1）

[English](LOW_FPS_FIX.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->

回帰確認で、P1のpublisherが何も処理していない状態でも1500msで待機を打ち切り、正常なリア2fps撮影を停止する問題が見つかりました。初回のlate publishにはSOF5が必要で、2fpsではSOF1から最低2000msかかります。最初のCOPY画像が届いても、その後STOP/error保持になり、通常の再利用ができなくなる場合があります。

`dph_run()`を、publishまたは明示的なcloseまで待つ方式に変更しました。待機中のpublisherはpending copyを所有しません。capture側のSOF/firmware進捗待ちには従来の期限があり、STOP/error時には`dph_close()`が両completionを起こします。既存のworker joinも維持します。`dph_send()`の実処理中copyに対する1500ms期限は変更していません。sensor、ISP、userspaceのalgorithmは不変です。

## ビルドと署名パッケージ

当時のP1 subtreeを、native ARM64 Alpine clang22.1.3と、照合済みの準備済みLinux6.18.28-mt81で新たにコンパイルしました。選択SDK125files/configとprovider10個を照合し、必要export201個は不足なし、KCFI有効です。この版ではkernel/userspace全体のclean buildは繰り返していません。入力hashとP1 moduleのSHA256は[S1_VALIDATION.json](S1_VALIDATION.json)に記録しています。

v6署名repositoryはmodules **0.2.4-r0**、meta **0.2.6-r0/r1**、config **0.1.3-r0/r1**、libcamera **0.7.2-r102**、PipeWire/GStreamer **1.6.8-r103**。APK3個を再発行し、v5の8個はbyte-identicalです。通常payloadの変更はP1 `.ko`だけで、meta依存版も更新しました。11APKとindexの署名を検証済み。[S1_PACKAGE_FIXES.json](S1_PACKAGE_FIXES.json)は[PRE_S1_PACKAGES.json](PRE_S1_PACKAGES.json)との対応を保持します。実ソースからのbuildと保全入力を使う梱包手順は[BUILD](BUILD.ja.md)を参照してください。

## 実機確認

既存の初代Duet SKU176のRAM起動、legacy DT aliasで試験しました。実機の8runtime package・203通常filesがv6署名payloadと一致し、ロードされたP1のbuild-ID noteも新ELFと一致します。

| カメラ／指定 | 完了frame数 | 実測median fps |
| --- | ---: | ---: |
| リア2fps | 48 | 2.00002 |
| リア2fps再起動 | 12 | 2.00002 |
| リアframe duration 850000 µs | 32 | 1.17650 |
| リア30fps | 60 | 30.01291 |
| フロント15fps | 30 | 15.01659 |
| 初回STOP後のリア2fps | 12 | 2.00002 |

有限撮影6sessionで合計194framesを完了しました。各回とも正常終了し、capture/input error=0、inputs idle、finalized、pending/ref=0、SCP offlineです。初回late publish前のSOF1で通常SIGINTを送り、**0.0645秒**で停止し、その後の2fps再利用も成功しました。

普通Snapshotを別processで2回起動し、前後のlive進捗、合計3回の切替、5枚のローカルJPEG撮影、正常Close/exit0を確認しました。再起動の間もPipeWire/WirePlumberは動作を継続しました。写真は実機のRAM filesystemだけに置き、公開成果物には含めません。この試験で校正済みの画質を受け入れたわけではありません。

最終状態はcamera app終了、capture/input error=0、inputs idle、finalized、pending/ref=0、SCP offline。eMMCのOS partitionはread-onlyを維持し、disk全体はread-onlyではありません。eMMCのOS・boot領域・firmwareは書き換えていません。

## ホスト回帰と限界

```sh
python3 tests/test-handoff-wait.py
python3 modules/mt8183-p1-public/tests/run_stop_gate.py
```

実handoff関数を使い、短時間／2000ms／4350msの初回待ち、待機中STOP、実処理期限後の所有権保持、closed/busy拒否の7casesがASan/UBSanでPASS。既存STOP/gate15casesもPASS。hostの合成時間試験と、上の実機測定は別の証拠です。

canonical DT実起動、外部OSのcold boot、耐久、sleep、高解像度、codec共存、完全なAF/色/ノイズ/flicker校正は再試験していません。以前の結果は[TESTING](TESTING.ja.md)に履歴として残します。
