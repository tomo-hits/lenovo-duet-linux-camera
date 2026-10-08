# 初代Lenovo Duet ChromebookのLinuxカメラ対応

[English](README.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->


初代Lenovo IdeaPad Duet Chromebook **SKU176**の前後カメラを、libcamera・PipeWire経由でSnapshotなどから使うためのドライバーと導入・復元ツールです。対象は **postmarketOS v26.06 / Alpine v3.24 / 6.18.28-mt81 / ARM64 / KCFI**です。

## 導入

導入には署名済みの更新アーカイブが必要です。[GitHub Releases](https://github.com/tomo-hits/lenovo-duet-linux-camera/releases)で配布状況を確認し、[導入・復元手順](docs/INSTALL.ja.md)に従ってください。cloneやGitHubの自動ソースアーカイブだけでは導入できません。

検証して展開した更新セットから、初回の起動設定を準備し、再起動して導入します。

```sh
sudo sh install.sh --prepare-boot
sudo systemctl reboot
# After reboot, return to the same extracted directory.
sudo sh install.sh
sudo systemctl reboot
```

保存したパッケージ・設定・起動構成へ戻す場合：

```sh
sudo sh restore.sh
sudo systemctl reboot
```

導入・復旧に必要な間は更新セットとバックアップを保管してください。前提条件、検査、旧バックアップの互換性は導入手順に記載しています。

## ソースと検証範囲

このソースは、30 fpsの要求を維持しながら後カメラの中間調を明るくするgamma 2.4を含みます。対応するlibcamera r103／meta 0.2.13のAPKセットは梱包検査と同じバイナリでの調整比較を実施していますが、**新APKセットの導入・復元は未試験です**。[版ごとの試験結果](developer/docs/TESTING.ja.md)を参照してください。

v1.0.1の実機試験では前後撮影、スピーカー再生、スリープ復帰、記録したOS対象への復元を確認しています。fpsは露光・処理条件で変わります。完全な画質・AF校正、長期耐久、別カーネル、hardware codecの同時利用は未確認です。[既知の制限](docs/KNOWN_ISSUES.ja.md)。

## 開発とライセンス

[ビルド・梱包・試験ガイド](developer/README.ja.md)に固定ソースと出典をまとめています。[ライセンス](LICENSE.ja.md) / [著作権表示](NOTICE.md)。部品の原ライセンスを保持しています。
