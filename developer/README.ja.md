# 開発者ガイド

利用者向けの導入・復元は[INSTALL](../docs/INSTALL.ja.md)、版ごとの実機結果と未確認事項は[TESTING](docs/TESTING.ja.md)を参照してください。現行ソースは後gamma 2.4を含みますが、新APKセットの導入・復元は未試験です。

[English](README.md) | 日本語 | [利用者向けREADME](../README.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->

ドライバー、ユーザー空間の修正、ビルド・梱包・試験、実験環境の準備をこのディレクトリにまとめています。通常の導入と復元は[利用者向け手順](../docs/INSTALL.ja.md)を使います。

## 作業別の入口

| 作業 | 手順 |
| --- | --- |
| 現行版をソースから作る | [ビルド・署名・更新セットへの接続](docs/BUILD.ja.md) |
| 別のカーネルへ対応する | [カーネル更新と再ビルド](docs/KERNEL_UPDATES.ja.md) |
| 開発時に個別操作を確認する | [手動導入の参考手順](docs/MANUAL_INSTALL.ja.md) |
| 実機実験の起動環境を用意する | [外部起動実験の準備](docs/EXTERNAL_BOOT.ja.md) |
| 内蔵OSの実験と復旧を確認する | [内蔵eMMCの準備と復旧](docs/INTERNAL_EMMC.ja.md) |
| 変更を検査する | [ホスト回帰試験](tests/README.ja.md) / [実機試験と結果](docs/TESTING.ja.md) |
| 処理の仕組みを知る | [構成と仕組み](docs/ARCHITECTURE.ja.md) / [パッケージ構成](packaging/postmarketos/README.ja.md) |
| 残作業を確認する | [既知の課題](docs/KNOWN_ISSUES.ja.md) |
| 出典と再配布条件を確認する | [出典](docs/PROVENANCE.ja.md) / [OSS配布資料](docs/OSS.ja.md) / [ライセンス](../LICENSE.ja.md) |
| ダウンロード用の更新セットを作る | [配布アーカイブの作成](docs/REPACKING.ja.md) |
| 過去版を調べる | [リリース記録](docs/RELEASE_NOTES.ja.md) / [過去版の再梱包](docs/REPACKING.ja.md) |

## ビルドから確認まで

1. ARM64ネイティブのAlpine環境と固定のソース・カーネル入力を用意します。
2. [BUILD](docs/BUILD.ja.md)に従い、5モジュール、libcamera/PipeWire、署名APKの順で作ります。
3. 対応する実験環境でルートの `install.sh` にパッケージ先と検証済みの署名鍵指紋を渡します。
4. [TESTING](docs/TESTING.ja.md)に従い、ビルド、認識、画像取得、画質を分けて記録し、復元も確認します。

各手順の `kit` は **この `developer/` の絶対パス**です。`repo` は `install.sh` がある一つ上のディレクトリを指します。例えば次の関係になります。

```sh
repo=/absolute/path/to/lenovo-duet-linux-camera
kit="$repo/developer"
```

| ディレクトリ | 内容 |
| --- | --- |
| `modules/` | 現行5モジュールと、比較・回帰試験に必要な過去実装 |
| `patches/`, `tuning/` | libcamera/PipeWire等のパッチと調整データ |
| `configs/` | 対象カーネルの設定・固定入力 |
| `scripts/` | ビルド、梱包、検証、更新処理の実装 |
| `packaging/` | 適合確認、DT・起動統合、有効化処理 |
| `tests/` | ホスト回帰試験と入力 |
| `docs/` | 手順、設計、出典、版ごとの検証記録 |
| `LICENSES/` | 原ライセンス本文 |

現行配布用P1は `modules/mt8183-p1-public/` です。同ディレクトリのREADMEは以前のUSB履歴を保持し、現行の内蔵モードの結果は[TESTING](docs/TESTING.ja.md)に記載します。他のP1ディレクトリは以前の実装と比較試験を保存しています。過去のJSON manifest・入力hashは記録時点の版を表し、特記がなければソースのパスは `developer/` を基準とします。旧archiveと新配置を混ぜないでください。

## 開発上の制約

公開版の対象はSKU176 / 正確な6.18.28-mt81 ARM64 KCFIと対応SCP firmwareです。内蔵eMMC向けに既存起動設定の準備・復元を含みますが、任意のOS構成には対応しません。別カーネル・別機種は別途検証し、実験前に元状態と復旧経路を保全してください。媒体の書込み前に機種・容量・シリアル・mountを照合し、必要なデータを別媒体へ保存します。

password・PIN・署名秘密鍵・カメラ画像を配布物へ含めません。認証は利用者の端末で行い、写真はローカルに保持します。元作者・出典・ライセンス・対象カーネルを記録してください。
