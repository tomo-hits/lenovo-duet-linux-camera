# 導入と元に戻す方法

[English](INSTALL.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->

**配布：** [GitHub Releases](https://github.com/tomo-hits/lenovo-duet-linux-camera/releases)にある完全な署名済み更新アーカイブを使います。配布がない場合は[開発者ガイド](../developer/docs/BUILD.ja.md)で署名済みセットを作成します。ソースだけでは導入できません。後gamma調整のAPKセット（libcamera r103／meta 0.2.13）の導入・復元は未試験です。

v1.0.1は設定外部変更の復元前保護と、未書込み起動準備の復旧修正を含みます。v1.0.0で作成したバックアップには設定の照合情報がなく、v1.0.1では自動復元できません。元の更新セットとバックアップを保持し、個別確認による復旧を行ってください。v1.0.0の処理が未復元の状態でv1.0.1を重ねて導入しないでください。保存対象を編集した後は旧復元スクリプトを使わないでください。[既知の制限](KNOWN_ISSUES.ja.md)を参照してください。

## 対応環境

導入ツールは初代Lenovo Duet **SKU176**、postmarketOS **v26.06 / Alpine v3.24**、**6.18.28-mt81 / ARM64 / KCFI**の内蔵OS用です。OSの新規インストールやカーネル更新は行いません。機種・カーネル本体・SCP firmware・起動ファイルの構成を検査し、異なる環境では停止します。

`curl`、Python 3、sudo、apk-tools 3、`vbutil_kernel` とOS付属のChromiumOS開発者用署名鍵が必要です。カメラ画像は端末内に保存します。適用中は電源を接続し、OS・パッケージ更新を同時に行わないでください。

## 1. GitHubから取得して検証

配布済みの署名付き更新セットのタグで下のプレースホルダーを置き換え、添付ファイル名も確認してください。Duetのターミナルで実行します。`curl` がなければ、先に `sudo apk add curl` で用意します。

```sh
version=REPLACE_WITH_AVAILABLE_RELEASE_TAG
mkdir "duet-camera-$version"
cd "duet-camera-$version"
curl -fL --retry 3 -o "lenovo-duet-camera-${version}.tar" \
  https://github.com/tomo-hits/lenovo-duet-linux-camera/releases/download/${version}/lenovo-duet-camera-${version}.tar
curl -fL --retry 3 -o SHA256SUMS \
  https://github.com/tomo-hits/lenovo-duet-linux-camera/releases/download/${version}/SHA256SUMS
sha256sum -c SHA256SUMS && tar -xf "lenovo-duet-camera-${version}.tar"
cd lenovo-duet-linux-camera
```

チェックサムがすべて `OK` になることを確認してから展開します。GitHubの「Source code」やcloneだけでは署名パッケージは揃いません。更新セットには導入・復元スクリプト、署名パッケージ、対応ソースを同梱しています。

## 2. 初回の起動設定を準備

カメラアプリを閉じ、同じフォルダーで実行します。

```sh
sudo sh install.sh --prepare-boot --check
sudo sh install.sh --prepare-boot
sudo systemctl reboot
```

元のカーネル領域32 MiB、起動ファイル、fstabとカメラ用設定の内容・権限を保存し、保存したファイルのhashを確認してからカメラのDTと隔離設定を適用します。カーネル・initramfs・起動引数は維持し、firmware・GPT・eMMCのhardware boot0/boot1は変更しません。起動領域への書込みは停電に対してatomicではありません。保存先を削除しないでください。起動不能時に備え、OSの復旧用バックアップは別端末にも保全してください。

既にこの更新セットで起動準備を済ませている場合は、この準備を繰り返さず次へ進みます。

## 3. カメラを導入

再起動後、通常ユーザーでログインし、同じ展開フォルダーに戻ります。

```sh
sudo sh install.sh
sudo systemctl reboot
```

元のパッケージ本体・版一覧・world・対象設定を保存した後、署名を検証して導入します。元のパッケージが端末にない場合は配布元から取得するのでネット接続が必要です。ユーザーは通常自動で選びます。特定できない場合だけ `--user ユーザー名` を付けます。

再起動後はSnapshotで前後を撮影できます。標準設定は1536 × 864 / 30 fpsを要求し、公開v1.0.0受入記録の連続取得は平均約24 fpsでした。カメラを閉じてからスリープしてください。撮影中は安全のためスリープを拒否します。[確認範囲と制限](KNOWN_ISSUES.ja.md)。

## 4. 元に戻す

カメラを閉じ、同じフォルダーで実行します。起動準備だけで止めた場合もこの手順です。

```sh
sudo sh restore.sh
sudo systemctl reboot
```

保存したパッケージ・world・対象設定を戻した後、起動領域・起動ファイル・fstab・隔離設定も戻します。保存対象の内容・権限・リンク・元の不在状態を照合します。写真やユーザーの文書は削除しません。全filesystemを過去へ戻す機能ではありません。

保存先は `/var/lib/duet-camera-update`、`/var/lib/duet-camera-boot-preparation`、`/var/lib/duet-camera-boot-transaction` です。導入・復元中は削除せず、復元後も復旧に必要な間は保管してください。

## エラー時

表示されたエラーを保存してください。非対応環境で検査を迂回しないでください。別のOS・パッケージ更新や保存対象への外部変更があると、安全のため復元を拒否します。STOP診断に失敗した状態で強制unload・強制kill・SCPの反復停止は行わないでください。起動できない場合や保存対象が一致しない場合は、別途保全したOSバックアップから復旧します。

既存の準備済み外部USB向け経路は開発者用の `--external-usb` で残していますが、v1.0.0の実機受入は内蔵OSを対象としています。
