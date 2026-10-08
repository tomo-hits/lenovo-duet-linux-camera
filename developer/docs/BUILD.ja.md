# 固定ソースからのビルド

後gamma調整のv1.0.2パッケージは[PUBLIC_PACKAGES](PUBLIC_PACKAGES.json)、以前の公開版は[PRE_REAR_BRIGHTNESS_PACKAGES](PRE_REAR_BRIGHTNESS_PACKAGES.json)に対応します。P1は[PUBLIC_P1_BUILD](PUBLIC_P1_BUILD.json)の同じELFを使用し、203のexportを検査した版です。[従来の受入](PUBLIC_ACCEPTANCE.json)と[今回の確認範囲](ACCEPTANCE_2026-10-08.json)を分けて参照してください。下のUSB/S1に関する結果は過去版の記録です。


[English](BUILD.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->

以下の新規ビルド手順は、新しい作業ディレクトリとstageを生成します。実機への投入、受入camera binaryのコピー、GUI rootfs payloadの利用、置き換えkernel Imageの生成は行いません。native **aarch64 Linux / Alpine v3.24 musl** と約8GBの空き領域が必要です。検証builderはclang/LLD **22.1.3**、KCFI、GCC **15.2**、Meson **1.11.1**、Samurai **1.2**（Ninja互換version **1.9**）、GStreamer **1.28.3**、apk-tools **3.0.8**。正確なpackage版は[BUILD_RECORD.json](BUILD_RECORD.json)にあります。

この手順は **6.18.28-mt81** 向けカメラパッケージの再現用です。「カーネル入力の準備と5モジュールのビルド」「libcamera/PipeWireのビルド」「署名APK作成」の3段階で進めます。新しいカーネルの場合は[KERNEL_UPDATES](KERNEL_UPDATES.ja.md)を参照してください。

## 作業ディレクトリ

`repo` は取得したリポジトリ、`kit` は開発用ソースの置き場所です。以降の例はこの変数を設定した同じシェルで実行します。`/inputs`、`/new` は手元の入力・展開先へ置き換えてください。出力先は新規ディレクトリを使います。

```sh
repo=/absolute/path/to/lenovo-duet-linux-camera
kit="$repo/developer"
work=/absolute/path/to/new-build-root
```

## 依存

信頼するAlpine/postmarketOS repositoryからnative compilerと依存を導入します。Alpineでは `eudev-dev`、検証したsystemd環境では既存の `systemd-dev` をlibudev providerとして使います。systemdと `libudev-zero-dev` を併用しないでください。

```sh
apk add build-base clang llvm lld bison flex perl openssl-dev xz zstd kmod \
 git meson samurai pkgconf python3 py3-jinja2 py3-yaml py3-ply \
 libevent-dev yaml-dev gnutls-dev elfutils-dev libdrm-dev \
 gstreamer-dev gst-plugins-base-dev alsa-lib-dev dbus-dev \
 libsndfile-dev readline-dev ncurses-dev sbc-dev
```

BusyBox modinfoではなくkmodの `/sbin/modinfo` を使います。kernel scriptはclang22.1.3を要求します。repositoryの既定版が変わったら対応版を固定してください。新しいcompilerを無条件に受け入れません。

## Linux sourceとABI入力

`https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.28.tar.xz` を取得します。[COMPLETE_SOURCES.json](COMPLETE_SOURCES.json)の完全ソースarchiveをすでに持っていれば、その中の同一tarballも使えます。署名済み更新セットは対応packageと完全sourceを含みます。[Releases](https://github.com/tomo-hits/lenovo-duet-linux-camera/releases)で配布状況を確認してください。[ダウンロード手順](../../docs/INSTALL.ja.md)を参照してください。全SHA512はscriptと `configs/kernel/APKBUILD` で検証します。同梱postmarketOS7patchをAPKBUILD順で適用し、matched.configを使って `olddefconfig modules_prepare` を実行。configのbyte一致と選択した125SDK sourceのhash一致を要求します。生成object/headerは新sourceから用意し、旧準備済みSDKはコピーしません。

正確なkernel APKとAPKINDEXは[postmarketOS v26.06/aarch64](https://mirror.postmarketos.org/postmarketos/v26.06/aarch64/)から取得します。このAPK内の署名は標準未信頼の `pmos@local-6a18a82e.rsa.pub`。配布OSが信頼する `build.postmarketos.org.rsa.pub` で公式index署名を確認し、そこに載るAPK control checksumとcontrol内のpayload SHA256を同梱verifierで検証します。APK全体のSHA256は `7729c6db0bff84ace07dd55e6f67afc8c3f82982989528d0a055631afb5afd4d`。検証に成功した場合だけ展開とImage生成へ進むよう、次のコマンドを連結しています。

```sh
python3 "$kit/scripts/verify-kernel-input.py" \
 linux-postmarketos-mediatek-mt81-6.18.28-r0.apk APKINDEX.tar.gz \
 /etc/apk/keys/build.postmarketos.org.rsa.pub &&
apk extract --allow-untrusted --destination /new/kernel-input \
 linux-postmarketos-mediatek-mt81-6.18.28-r0.apk &&
gzip -dc /new/kernel-input/boot/vmlinuz > /new/kernel-input/Image
```

`--allow-untrusted` は上記の独立した署名／control／data照合が成功した後に、APK内の未認識keyだけを迂回するものです。入力検証を省略しません。APK内にはImage、`boot/System.map`、`boot/config`、`usr/lib/modules/6.18.28-mt81` があります。固定10providerは独立取得分でも一致を確認。相対pathと `.ko.zst` を保持します。

| 入力 | SHA256 |
| --- | --- |
| 展開したImage | cce4914459d15558b54640f360e0a58476edfa74595fe30143d29dce7049d5a1 |
| System.map | 3757e93b5d3cfc162350534b637810fbc0807fed47b644cfc888bd372c664329 |
| public boot config | 780e607deea636e4ffba18ac49be4b36926d0c2e1308d380d6cb764c604d6756 |
| matched.config | 141ececb5afef42ba36a5794c4cf0508562b5187d32cf6bc0ab2a9c6d2cd80f4 |

```sh
sh "$kit/scripts/build-kernel.sh" "$work/kernel" \
 /inputs/linux-6.18.28.tar.xz /new/kernel-input/Image \
 /new/kernel-input/boot/System.map /new/kernel-input/boot/config \
 /new/kernel-input/usr/lib/modules/6.18.28-mt81 3
```

新SDKにもModule.symversはありません。builtin auditorがImageの実ARM64 PREL32 exportsとSystem.map/GPL区分/namespaceを検証し、required auditorがpublic media/SCP ELF exportsを読みます。このconfigでMODVERSIONSを無効にしている場合だけCRC0を使います。必要exportsをKBUILD_EXTRA_SYMBOLSへ渡し、偽のkernel symversや不足exportの無視はしません。新OV8856 ELFにprivate sensor exportが無いことも確認します。現行内蔵対応P1は必要203exportsです。過去USB版は202、S1版は201で、各記録の不足は0でした。監査結果とlogsはoutputへ残します。

## userspaceとpatch順

```sh
sh "$kit/scripts/build-userspace.sh" "$work/userspace" 3
```

libcameraは固定commit `c0049ea0605c1492c99b8f82bb04661a04cf1bf1`。complete patchだけを適用し、.tarball-versionをコピーして、同梱Meson設定でSimple/SoftISP/camを有効にします。新AF/flicker/statistics4filesも含みます。変更sourceを `libcamera-source-hashes.json` と照合し、tuning YAML3個を導入。IPA用鍵は新しく生成し、その署名と公開鍵を使います。秘密鍵は配布しません。

PipeWire1.6.8は固定commit `b741e0c74f5436f0c925f7741140db0efd32cf4e` に次の順で適用します。

1. `pipewire-1.6.8-libcamera-complete.patch`
2. `pipewire-1.6.8-gst-copy.patch`
3. `pipewire-gst-state-change-backport-v2.patch`
4. `pipewire-link-reset-preparing-upstream-v2.patch`
5. `pipewire-gst-real-copy-default-v3.patch`

systemd/logindは受入eudev RAM環境に合わせ無効化します。依存 `cmd:udevadm` はどちらのudev providerも許可。標準client.conf/client.conf.availをclient librariesへ含めます。core・有効SPA・Gst pluginをfresh libcameraとtarget Gst ABIに対してビルドし、再配布用libcamera pkgconfigのprefixを `/usr` に戻します。qcamはこのAPKには含めず、高解像度の過去試験は別途作成したGUI版です。

完全ソースarchiveにはpatch適用後のlibcamera/PipeWireと元Linux tarballを含みます。これは通常recipeの固定checkout＋patchと対応する配布ソースです。配布用の変更codeには日付付きコメントだけを加え、元build入力hashはMODIFICATIONS.jsonに保持します。S1ではP1のpublisher待機を修正しています。sensor/ISPのalgorithmと完全userspaceソースは不変です。詳細は[OSS](OSS.ja.md)。

## 署名APK

後gammaパッケージ集合の署名・全payload再展開・63 ELFのbyte一致・ホスト試験の結果は [REAR_BRIGHTNESS_PACKAGING](REAR_BRIGHTNESS_PACKAGING.json) に記録しています。

v1.0.2候補は後gamma2.4を含むlibcamera4パッケージを `0.7.2-r103`、固定依存を持つmeta2パッケージを `0.2.13-r0/r1`とします。他5 APKは以前のv1.0.1とbyte同一です。変更したtuningを既存r102と同じ名前・版で再配布しません。[package-rear-brightness.py](../scripts/package-rear-brightness.py)は検証済みの旧パッケージからELF本体を保ち、tuningと版・依存指定を限定して再梱包する経路です。下のfresh build経路はソースからELFも生成するため、再利用版とのbit同一性や実機受入を自動的に継承しません。署名・hash・対応完全ソースを照合し、新APKセットの導入は未試験として扱います。

apk-tools3.0.8のmkpkg/mkndxを使います。ローカルRSA秘密鍵と公開鍵のpathを明示的に渡します。以下はpathの例で、鍵を同梱したり秘密鍵を取り出したりするrecipeではありません。

```sh
python3 "$kit/scripts/package-fresh.py" \
 --kernel "$work/kernel" --userspace "$work/userspace" \
 --output "$work/packages" \
 --sign-key /private/local-signing-key.rsa \
 --public-key /public/local-signing-key.rsa.pub
```

出力は11署名APK、署名packages.adb、公開鍵、package SHA256 manifestです。modules **0.2.8-r0**、meta **0.2.13**、config **0.1.3**、libcamera **0.7.2-r103**、PW/Gst **1.6.8-r104**。fresh ELFと公開helperを使い、以前のAPK payloadを抽出する工程ではありません。IPA秘密鍵、work tree、provider kernel binary入力、署名秘密鍵は配布しないでください。

過去modules 0.2.5のUSB版P1をnative Clang/LLD 22.1.3・KCFIで再ビルドしました。SDK125ファイルと10providerは不変です。生成モジュールは203,704バイトで、hashとソースの対応は[P1_USB_FIX.json](P1_USB_FIX.json)に記録します。差分の再梱包ではmodules APK内のP1と有効化処理、meta2個の依存指定だけを変更し、他8APKはbyte単位で不変です。11APK/indexの署名と通常ファイル288個を検証しました。この修正のためのカーネル・起動イメージ再生成は行っていません。

build成功だけではsensor認識・画像取得・画質を証明しません。過去modules 0.2.5は実USBへの導入、コールドブート、自動有効化、前後2台の列挙を確認しました。前後撮影と新プロセスでの再利用も確認済みです。画質の完全な校正は未完です。[TESTING](TESTING.ja.md)を参照してください。

## 自作パッケージを適用する

ビルド時に使った手元の信頼済み公開鍵から、指紋を取得して独立に保管します。配布物に同梱された未検証の鍵を、そのまま期待値の根拠にはしません。

```sh
openssl pkey -pubin -in /public/local-signing-key.rsa.pub -outform DER | sha256sum
```

出力は `$work/packages` です。検証済みの対象環境へパッケージとリポジトリを置き、**自分の署名公開鍵のSPKI DER SHA256を独立に確認した値**を指定します。下の例の指紋はその値へ置き換えてください。利用者向けの配布鍵とは区別します。

```sh
sudo sh "$repo/install.sh" --packages "$work/packages" \
 --expected-key-sha256 YOUR_INDEPENDENTLY_VERIFIED_SPKI_SHA256
```

導入処理はこの指紋、APK署名、対象環境を確認し、元のパッケージ・設定を保存してから適用します。成功したら案内に従って再起動します。元へ戻す場合もルートのシェルを使います。

```sh
sudo sh "$repo/restore.sh"
```

`--user NAME` でデスクトップユーザーを明示できます。保存済みの導入前状態から復元するため、復元時に自作パッケージの鍵を指定し直す必要はありません。詳細は[利用者向け導入・復元](../../docs/INSTALL.ja.md)。ソースのビルドだけでは、対象の起動環境は用意できません。

個々のAPK操作を調査する場合は[手動導入](MANUAL_INSTALL.ja.md)、別カーネルへの対応は[カーネル更新と再ビルド](KERNEL_UPDATES.ja.md)、保存済み入力から過去版を再現する場合は[再梱包手順](REPACKING.ja.md)を参照してください。
