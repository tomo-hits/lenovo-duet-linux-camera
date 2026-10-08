# 既知の制限

[English](KNOWN_ISSUES.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->


対象は初代Duet SKU176、正確なpostmarketOS v26.06 / Alpine v3.24 / 6.18.28-mt81 ARM64 KCFIカーネルと対応SCP firmwareです。結果は[TESTING](../developer/docs/TESTING.ja.md)に記した版・環境に限定します。

- **後カメラの明るさ：** ソースはgamma 2.4、以前のv1.0.1パッケージは2.2です。同じバイナリでの比較では中間調の視認性が改善しました。libcamera r103／meta 0.2.13の新APKセットの導入・復元は未試験です。低照度での露光・gain上限は残ります。
- **fps：** 標準要求は1536 × 864 / 30 fpsです。v1.0.0の連続取得は約24 fps、別のGamma control試験は受信buffer時刻から約30.01155 fpsでした。Snapshotの表示fpsや常時30 fpsを保証しません。
- **スリープ：** カメラを閉じてから実行します。撮影中は意図的にsuspendを拒否します。v1.0.1でdeep sleepと復帰後撮影を確認していますが、日単位の耐久は未確認です。
- **画質・AF：** 色・ノイズ・フリッカー・距離ごとのAF校正は未完です。LensPositionは非表示、自動フリッカー検出はありません。
- **音声・関連機能：** PipeWireはBlueZ、JACK SPA、FFmpeg、Vulkan、ROC、libmysofa、EVLを無効にしています。v1.0.1では導入前と前カメラ撮影中のスピーカー再生を聴取確認しました。マイク録音や全関連機能は未確認です。カメラ/SCP隔離により競合するhardware codec・MDP・JPEGも無効になります。
- **復旧：** 更新セットと元の起動・パッケージ状態を保管してください。STOP失敗、無関係なパッケージ・world変更、保存対象への外部変更があれば復元を拒否します。起動領域の書込みは電源断に対してatomicではなく、全filesystemを戻す機能でもありません。
- **旧バックアップ：** v1.0.0のパッケージバックアップは設定の照合情報を持たず、新スクリプトで自動復元できません。元kitとbackupを保持して個別確認で復旧します。旧transactionが未復元のまま重ねて導入したり、設定編集後に旧復元処理を使ったりしないでください。
- **別環境：** 別機種・別カーネル・個別更新したmedia stackは対象外です。旧外部USB試験は当時のパッケージの結果で、現行セットを検証するものではありません。

[導入・復元](INSTALL.ja.md) / [開発上の詳細](../developer/docs/KNOWN_ISSUES.ja.md) / [カーネル移植](../developer/docs/KERNEL_UPDATES.ja.md)。
