# 手動導入の参考手順（開発者向け）

[English](MANUAL_INSTALL.md) | 日本語 | [開発者ガイド](../README.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->

**版指定の範囲：** 以下のAPKコマンドはv1.0.2パッケージ集合、またはこのbranchのfresh build用です。libcamera `0.7.2-r103`／meta `0.2.13-r0/r1`を使います。配布済みの更新セットを通常導入する場合は、その更新セットに同梱された `install.sh` と[利用者向け手順](../../docs/INSTALL.ja.md)を使ってください。新候補APKの導入・復元は未試験です。

通常の導入と復元はルートの `install.sh` / `restore.sh` を使います。[利用者向け手順](../../docs/INSTALL.ja.md)と、自作署名パッケージを渡す[BUILD](BUILD.ja.md)を参照してください。

ここでは、適合検査・署名検証・APK操作を調査するための個別手順を記録します。**この手動経路には導入前状態の自動保存・復元がありません。** 開発者が別途バックアップと復旧経路を用意した場合だけ使います。

### 必要なもの

| 項目 | 対応する条件 |
| --- | --- |
| 機種 | 初代Lenovo IdeaPad Duet Chromebook、`google,krane-sku176` |
| OS | 外部メディア上の専用テスト環境、postmarketOS v26.06 / Alpine v3.24、ARM64 musl |
| カーネル | **6.18.28-mt81**の特定のビルド。バージョン名だけの一致では不十分 |
| ファームウェアとデバイスツリー | 対応するSCPファームウェアと、有効化済みのカメラ構成。下の手順で確認 |
| パッケージ管理・検査ツール | apk-tools 3、Python 3、OpenSSL、kmod |
| カメラを使う環境 | WirePlumber **0.5.15-r0**、PipeWire、デスクトップのカメラポータル、Snapshot |
| 復旧手段 | 別途起動できる復旧環境と、復元可能な外部OSのバックアップ |

導入すると、システムの**libcameraとPipeWireのライブラリーを置き換えます**。同梱するPipeWireではBlueZ、JACK、FFmpeg、Vulkan、ROC、libmysofa、EVLを無効化しています。置換後の既存音声機能やデスクトップの追加機能は確認できていません。専用のテスト環境を用意してください。詳しくは[既知の問題](KNOWN_ISSUES.ja.md)を参照してください。

署名パッケージの配布状況は[Releases](https://github.com/tomo-hits/lenovo-duet-linux-camera/releases)で確認します。対応する内蔵OSの導入は[利用者向け手順](../../docs/INSTALL.ja.md)を使ってください。以下の手動操作は準備済みの外部テストOS用です。上記の版に対応する検証済みv1.0.2候補、またはこのbranchの[BUILD](BUILD.ja.md)で作成した署名済みパッケージを使います。以前のv1.0.1パッケージは以下の版指定に一致しません。このリポジトリの取得だけでは、カーネル、ファームウェア、OS、APKは揃いません。自分でビルドした場合の出力先（`$work/packages`）も、以下の手順と同じ構成です。

### 1. インストール前に環境を確認する

外部のテストOS上で、デバイスツリーを読み取れるrootとして実行します。2つのパスを、手元のソースと展開済みパッケージのディレクトリに置き換えてください。この後の手順でも同じ変数を使います。

```sh
kit=/absolute/path/to/lenovo-duet-linux-camera/developer
candidate=/absolute/path/to/packages
python3 "$kit/packaging/postmarketos/duet-camera-check.py" \
 --abi "$kit/packaging/postmarketos/kernel-abi.json"
```

正常終了し、`"compatible": true` と表示された場合だけ進めます。この検査は、実行中のカーネルビルド、機種、デバイスツリー、ファームウェアを読み取ります。パッケージの導入、ドライバーの読み込み、起動ファイルの変更は行いません。不一致を通すために記録済みハッシュを書き換えないでください。新しいカーネルを使う場合は[カーネル更新と再ビルド](KERNEL_UPDATES.ja.md)へ進みます。

外部OSを変更する前に、そのOSの復旧手順に従ってバックアップを作成し、復元できることを確認してください。保存先は別のストレージにします。起動ファイル、導入済みパッケージの本体と版、`/etc/apk/world`、リポジトリ設定、システムとユーザーのPipeWire・WirePlumber設定を保全します。`world`やバージョン一覧のコピーだけでは、置き換えたライブラリーを復元できません。過去の外部USB試験では、当時の旧候補を元の全パッケージ・world・設定へ復元し、別の候補でも元の838パッケージ・world・設定への復元が成功しました。その記録では復元後のUSB起動は未確認です。これはv1.0.2候補の実機受入ではありません。

### 2. パッケージの出所を検証する

配布アーカイブを使う場合は、**展開する前に**、別途入手した配布一覧のSHA256と照合してください。その後、署名公開鍵の指紋とAPKの署名を確認します。以下の鍵のパスは配布パッケージ用です。自分でビルドした場合は、梱包時に指定した公開鍵のファイル名へ置き換えてください。

```sh
public_key="$candidate/keys/pmos@local-6ab3c683.rsa.pub"
openssl pkey -pubin -in "$public_key" -outform DER | sha256sum
apk verify --keys-dir "$candidate/keys" "$candidate"/repo/aarch64/*.apk
```

配布用の開発鍵のSPKI DER SHA256は、次の値と一致する必要があります。

```text
94d05c05d71e63aa74b0a2f11a4f4e3d8138e701daf5fe4f95e980b8fef73cd9
```

自分で署名したビルドでは、梱包時に指定した公開鍵を`public_key`に設定し、自分で別途記録した指紋と照合してください。別の鍵に上記の指紋は使えません。指紋や署名の検査に失敗したら中止し、`--allow-untrusted`で検証を回避しないでください。

### 3. 対応するパッケージ一式を導入する

カメラアプリを通常操作で閉じ、デスクトップのサービス管理方法に従ってPipeWireとWirePlumberを停止してください。手順4まで両方を停止したままにします。

この初回導入の手順では、カメラドライバーが読み込まれていないことが必要です。確認します。

```sh
lsmod | grep -E '^(mt8183_p1|duet_p1_video_raw|mtk_seninf|ov02a10|ov8856|dw9768) '
cat /sys/class/remoteproc/remoteproc0/state
```

最初のコマンドで該当するモジュールが表示されず、次のコマンドで`offline`と表示されることを確認します。ドライバーが読み込まれている場合は、アプリを通常終了し、カメラを自動有効化しない準備済み外部環境へ再起動してから、この手動経路を使ってください。撮影中やSTOPに失敗したドライバーを強制的に取り外さないでください。APKの導入だけでは、メモリー上で動いているドライバーは置き換わりません。

以下の版指定は**v1.0.2パッケージ集合／このbranchのfresh build専用**で、**1536 × 864、30 fps**を選びます。まず変更予定を表示し、予想外のOS・カーネル更新や無関係なパッケージ削除がないか確認します。メタパッケージが対応するドライバー、設定、ユーザー空間の部品を選びます。ライブラリーの版も明示することで、既存の`world`で固定された版を置き換えます。

```sh
install -m0644 "$public_key" /etc/apk/keys/ &&
apk --repository "$candidate/repo/aarch64/packages.adb" add --simulate \
 duet-camera=0.2.13-r1 libcamera=0.7.2-r103 libcamera-ipa=0.7.2-r103 \
 pipewire-libs=1.6.8-r104 gst-plugin-pipewire=1.6.8-r104
```

最初のコマンドは検証済みの鍵を信頼対象に追加します。[apkのシミュレーション](https://github.com/alpinelinux/apk-tools/blob/v3.0.8/doc/apk.8.scd)ではパッケージは変更されません。変更予定に問題がなければ、導入して有効化します。

```sh
apk --repository "$candidate/repo/aarch64/packages.adb" add \
 duet-camera=0.2.13-r1 libcamera=0.7.2-r103 libcamera-ipa=0.7.2-r103 \
 pipewire-libs=1.6.8-r104 gst-plugin-pipewire=1.6.8-r104 &&
duet-camera-check &&
duet-camera-activate --external-usb
```

15 fpsを選ぶ場合は、**両方**のパッケージ操作で`duet-camera=0.2.13-r0`を使います。暗い場面では、フレームレートを下げると露光時間を長くできます。導入と有効化はカーネル、起動ファイル、ファームウェアを書き換えません。外部USBでの有効化は、root/bootが同じUSB上にあり、内蔵eMMC全体が読み取り専用であることを検査します。続いて互換性とSCPの停止状態を確認し、P1を`external_usb=1`で読み込みます。このパラメーターは既定で無効で、起動サービスも同じ補助処理を経由します。すでに読み込まれた旧ドライバーを置き換える機能はありません。

### 4. 前後のカメラを確認する

通常ユーザーのPipeWire・WirePlumberセッションを再開します。WirePlumberは標準の`main`プロファイルを使います。`/etc/wireplumber`やユーザー設定が、同梱のカメラ設定より優先される場合があります。カメラアプリはrootで実行しないでください。

1. デスクトップのユーザーで`cam -l`を実行し、**カメラが2台**表示されることを確認します。アクセス権で失敗した場合は、そのデスクトップの通常の`video`グループ・udev方針で修正し、全ユーザーにデバイスへの書き込みを許可しないでください。
2. Snapshotを開き、前後のライブ映像を確認して、それぞれ写真を撮ります。写真はローカルに保持します。
3. Snapshotを通常操作で閉じ、もう一度開いてカメラの切り替えを確認します。最後に閉じた後、SCPが`offline`へ戻ることを確認します。

一覧への表示で確認できるのは認識、写真の保存で確認できるのは画像取得です。どちらも画質の校正完了を示すものではありません。失敗した場合はエラーを保全し、[動作確認](TESTING.ja.md)と[既知の問題](KNOWN_ISSUES.ja.md)を参照してください。強制的なドライバーの再読み込みを繰り返さないでください。

### 更新と復旧

**カーネルやカメラパッケージを更新する前に**、[カーネル更新と再ビルド](KERNEL_UPDATES.ja.md)を確認してください。現行パッケージに自動再ビルド機能はありません。libcamera、PipeWire、カメラドライバーを別々に更新した版で混在させないでください。

導入や動作確認に失敗したら、アプリを通常終了してセッションマネージャーを停止します。ドライバーが正常に停止できない場合は、強制的に取り外さず、外部OSの復旧経路を使ってください。元のパッケージ一式、設定、対応する起動ファイルを含む外部OSのバックアップを復元してから、通常利用へ戻します。`duet-camera`を削除するだけでは、置き換えたlibcamera・PipeWireは**元に戻りません**。

動作中やSTOP失敗のsessionはsuspendを拒否します。現行内蔵対応codeは正常停止またはidle未公開状態をsleep前に退役させますが、このAPKセットの外部手動導入でのsleepは未試験です。[既知の課題](KNOWN_ISSUES.ja.md)を参照してください。

### パッケージの版と対象範囲

このbranchのfresh recipeはmodules 0.2.8-r0 / meta 0.2.13-r0/r1 / libcamera 0.7.2-r103 / PW・GStreamer 1.6.8-r104を生成します。v1.0.2の限定再梱包も同じ版を使います。以前のv1.0.1やv6のAPKは上記の版指定に使えません。版指定はパッケージ集合の識別で、配布物の公開状況を示しません。圧縮SCP firmwareはzstdで展開して検査し、書き換えません。Pulseサーバーのmoduleは同じ1.6.8の配布側pipewire-pulseが所有する構成です。この新候補の外部手動導入での音声動作は未確認です。v1.0.1の内蔵試験でのスピーカー聴取とは区別します。
