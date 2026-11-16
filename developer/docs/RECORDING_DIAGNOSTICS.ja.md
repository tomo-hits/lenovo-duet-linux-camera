# 録画のフレーム間隔を調べる（Issue #1）

[English](RECORDING_DIAGNOSTICS.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->

Snapshotの録画時のカクつきは調査中です。この変更は配信記録の誤りを直し、
計測手段を追加するもので、録画性能の改善を実証したものではありません。
署名済みv1.0.2パッケージは変更していません。新しいドライバ診断には、
[ビルドガイド](BUILD.ja.md)に沿った対応カーネル向け候補ビルドと、通常の導入・有効化手順が必要です。

## 同じ条件で比較する

Snapshot・libcamera・PipeWire/GStreamer・モジュール・カーネルの実際のバージョン、
前後カメラ、照明、合意された解像度・fps・画素形式、エンコーダと設定、再生アプリを記録します。
SoftISPの実装とEGLレンダラーも確認します。`SWIspWorker`というスレッド名だけでは、
CPUデベイヤ・ハードウェアEGL・ソフトウェア描画を区別できません。

前後カメラで、プレビューのみ、エンコードなしのPipeWire/GStreamer受信、録画を同じ条件で比較します。
スレッド別CPU、温度・スロットリング、キュー・QoS、バッファ返却時間、書き込み負荷を記録し、
配信元とエンコーダ・muxerのPTSを比較します。公称fpsやセンサー割り込み数だけでは判定しません。
映像は端末内に保持します。

配布WirePlumber設定は解像度とfpsを固定します（通常1536x864/30 fps）。
SPAパッチはそのサイズだけを提示し、別サイズを拒否します。下流のcaps filterだけでは変更できません。
15 fps・低解像度の比較は、検証した配信設定への一時変更と通常の停止・再開で行い、終了後に復元します。
出力解像度を下げてもRAW撮影サイズが同じなら、RAW展開の負荷は減らない場合があります。

## ドライバの配信カウンタ

debugfsが利用できる場合、候補ドライバの
`/sys/kernel/debug/mt8183_p1/<platform-device>/video_result`を読み取れます。
root専用の読み取り専用診断で、数値だけを返し、ストリームを起動しません。
録画区間の前後で、`capture_result`・`session_result`と合わせて記録します。

| フィールド | 意味 |
| --- | --- |
| `stream_id` | フロントエンドの起動試行。同じIDの中で差分を比較する |
| `running` | フロントエンドが受け付け中か。センサーのフレーム到着は保証しない |
| `delivered` | このストリームでVB2へ正常完了したバッファ数 |
| `no_buffer` | 稼働中に受信バッファ不足で捨てたフレーム数 |
| `stopping` | 受付停止後に捨てたフレーム数 |
| `errors` | 入力フレームの不正、またはバッファの準備・変換・完了処理で発生したエラー数 |
| `total_delivered`・`total_dropped` | 再開後も保持する累積値。破棄にはバッファ不足と停止中の両方を含む |

ストリーム別カウンタは開始コールバックがフレームを生成する前にリセットします。
開始失敗でもIDが進む場合があります。STOPログはworker終了後の値を出力します。
VB2完了はSnapshotへの到着やエンコード成功を意味しません。

バックエンドは配信試行の完了と実際のVB2配信を区別します。通常のフレーム破棄でも
プロトコル処理は完了させ、配信成功として数えたり、撮影エラーに変えたりしません。
`archived`はプロトコル上の回収数で、アプリの受信数ではありません。
DMAコピー・一致比較・所有権確認、SCP隔離、通常STOP処理は維持します。

## 端末内の保存動画を解析する

Python 3とffprobeがある端末で、`developer`ディレクトリから実行します。

```sh
python3 scripts/analyze-video-timing.py --video /path/to/recording.webm
python3 scripts/analyze-video-timing.py --video /path/to/recording.webm --expected-fps 30/1
```

`--expected-fps`には実際に合意・確認した目標値を指定します。
ffprobeのJSONを`--json /path/to/frames.json`または標準入力の`--json -`で渡すこともできます。

```sh
ffprobe -v error -select_streams v:0 -show_frames -show_entries frame=best_effort_timestamp_time,pts_time -of json /path/to/recording.webm > /tmp/duet-recording-frames.json
python3 scripts/analyze-video-timing.py --json /tmp/duet-recording-frames.json
```

出力は表示時刻の間隔と、異常・欠落したタイムスタンプの集計です。
期待間隔に対する欠落数の推定は診断用で、可変fps、時刻丸め、エンコーダの判断などの影響を受けます。
一定間隔でも画像内容が重複している可能性があり、時刻の欠落だけで撮影・変換・録画のどこに原因があるかは確定しません。

## 性能改善を判定する前に

前後カメラで宣言したfpsの録画を繰り返し、保存動画の間隔と見た目の動き、遅延の蓄積を確認します。
早期停止、正常終了、新プロセスでの再開、前後切り替えを試し、既存のSTOP・SCP所有権の終了状態を確認します。
15 fpsで安定しても、30 fps録画の成功とは別に評価します。

ホスト試験は[テストガイド](../tests/README.ja.md)にあります。合成バッファ・provider応答を使う試験であり、
対応するARM64/KCFIビルドや実機録画の代わりにはなりません。
