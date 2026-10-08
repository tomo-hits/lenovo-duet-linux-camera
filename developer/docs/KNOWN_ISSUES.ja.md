# 開発上の制限

[English](KNOWN_ISSUES.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->


現行ソースとパッケージの対応は[PUBLIC_PACKAGES](PUBLIC_PACKAGES.json)、[PUBLIC_P1_BUILD](PUBLIC_P1_BUILD.json)、[COMPLETE_SOURCES](COMPLETE_SOURCES.json)です。[TESTING](TESTING.ja.md)は現行tuning比較と過去版実機結果を分けています。r103／meta 0.2.13の新APKセットの導入・復元は未試験です。

P1の初期runtime-PM参照は非同期で解放します。アプリ閉鎖後のsuspendは、idle未公開状態または正常停止sessionについて必要な所有権検査後だけ退役します。撮影中は拒否し、復帰後は従来のCAM OFF→ONを検証します。有効化はmodprobe成功に加えてplatform bindとvideo endpointを確認します。

正常Closeは停止済みDMAを再利用用に保持します。STOP失敗ではDMAを保持して再利用を拒否します。強制unload・参照の強制解放・アプリの強制終了は復旧手順ではありません。kernel ABI・firmware検査は固定です。codec/SCP同時利用、任意hot unplug、別機種・kernel、media stackの個別更新は非対応または未検証です。利用者のWirePlumber方針が標準設定より優先される場合があります。

fresh buildはpath・debug情報・新IPA鍵でbyteが変わり得ます。各buildの署名とsource/payload対応を別に検証してください。完全な画質・AF校正と長期耐久は未完です。[利用者向け制限](../../docs/KNOWN_ISSUES.ja.md) / [kernel移植](KERNEL_UPDATES.ja.md)。
