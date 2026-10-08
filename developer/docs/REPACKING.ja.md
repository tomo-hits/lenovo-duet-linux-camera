# 配布アーカイブの作成（開発者向け）

[English](REPACKING.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->

## 後gammaのパッケージ集合（v1.0.2）

この候補は後gamma2.4を含むlibcamera `0.7.2-r103`／meta `0.2.13-r0/r1`の6 APKを再梱包し、他5 APKをbyte同一で保持します。新しい[package manifest](PUBLIC_PACKAGES.json)と[完全ソース](COMPLETE_SOURCES.json)を使用し、旧版は[package記録](PRE_REAR_BRIGHTNESS_PACKAGES.json)／[source記録](PRE_REAR_BRIGHTNESS_COMPLETE_SOURCES.json)に保存します。旧版のAPK・archive・Release assetsを上書きしません。

配布archive・外側SHA256SUMS・版とsource/package対応のmanifestを揃えます。すべての内容・署名・hashを確認し、対応するソースとともに同じReleaseへ提供します。公開済み版の取得状況は[Releases](https://github.com/tomo-hits/lenovo-duet-linux-camera/releases)で確認してください。

[今回の実機記録](ACCEPTANCE_2026-10-08.json)は同じELFと候補YAMLの確認です。新しい署名APKセットの導入・復元は未試験であり、梱包検証と区別します。

## 現行の更新アーカイブ

利用者には、`install.sh`、`restore.sh`、補助コード、署名済みパッケージ、対応ソースを一つのアーカイブで渡します。GitHubのソースZIPだけではAPKは揃いません。ソースのチェックサムを更新し、native梱包でAPK署名を検証してから、新しい配布ファイルを作ります。

```sh
repo=/absolute/path/to/lenovo-duet-linux-camera
kit="$repo/developer"
python3 "$kit/scripts/build-update-bundle.py" \
 --packages /absolute/path/to/verified-packages \
 --complete-sources /absolute/path/to/verified-complete-sources.tar.xz \
 --output /absolute/path/to/new-camera-update.tar
```

作成ツールは、リポジトリ直下の`SHA256SUMS`にあるソースと、指定したパッケージ・対応ソースだけを収録します。hash不一致、不正なpath、署名鍵の不一致、既存出力を拒否します。秘密鍵やパッケージのstagingディレクトリは収録しません。既定の作者鍵では現行ソースに対応する`PUBLIC_PACKAGES.json`との完全一致も要求し、`USB_PACKAGES.json`は以前のUSB記録として保持します。bundle metadataの`hardware_acceptance: false`は、梱包処理がarchive内容を検証し、実機試験は行わないことを表します。過去の[PUBLIC_ACCEPTANCE](PUBLIC_ACCEPTANCE.json)を保持し、[今回の追加確認](ACCEPTANCE_2026-10-08.json)と区別します。署名済bytes一致だけを新APK導入の実証とは扱いません。開発者自身の鍵で署名した場合は、[BUILD](BUILD.ja.md)と同様に独立確認したSPKI指紋を`--expected-key-sha256`へ指定します。その配布物を導入する際にも同じ指紋指定が必要です。

`BUNDLE.json`へソース・パッケージの対象を記録し、`BUNDLE_SHA256SUMS`で自身を除く全収録ファイルを照合できます。公開時はアーカイブ全体のSHA256も別添します。梱包検査はAPK署名検証や実機確認の代わりにはなりません。直下のchecksumは配布するソース版に合わせます。過去JSONの入力hashは当時の版を示し、記録内pathは`developer/`基準として扱います。

## 保全済みの梱包手順


以下は、別途保全した入力と対応する署名鍵を必要とする過去版の再現手順です。導入やソースからの新規ビルドには使いません。現行ソースのビルドは[BUILD](BUILD.ja.md)、対応済み環境への導入は[INSTALL](MANUAL_INSTALL.ja.md)から進めてください。以下の例では `kit` をソースのディレクトリ、`work` を新しい出力先に設定します。

## 公開レビュー修正版の梱包

S1修正前の保全v5は、下の別recipeで署名済みv4を検証してからcamera APK5個のDT helper・著作権表示だけを更新したものです。全ELF、設定内容、userspace APK6個を保持し、元版は変更しません。実ソースからの新規buildはBUILDのpackage-fresh.pyを使います。元archiveのSHA256は `ce5f22c79a8c22a700b5721f1efca0de40b7f1a16280c0921653cb720385ed39` です。

```sh
python3 "$kit/scripts/package-release-fixes.py" \
 --accepted-repository /absolute/path/to/extracted-fresh-v4 \
 --output "$work/review-packages" \
 --sign-key /private/local-signing-key.rsa \
 --public-key /public/pmos@local-6ab3c683.rsa.pub
```

## 保全v5入力からS1修正版を梱包

配布v6は照合済みの準備済みkernelに対して現在のP1 C/H全体を新たにコンパイルし、署名v5のP1 ELFだけを置き換えています。この限定recipeは任意で、ソース全体からのbuildにはBUILDのkernel/userspace recipeを使います。保全v5 archiveのSHA256 `96ec65037ad726a6e339fb6520e611adb8491ddd22643460b5dcf2ac2c5825b4` を展開前に照合してください。scriptはv5の11APK hash・公開鍵を固定照合し、P1 payload以外の変更を拒否して、新署名とindexを検証します。既存出力pathは拒否します。署名には対応するローカル秘密鍵が必要で、鍵は同梱していません。

S2修正では、出力を作る前に付属の `payload-hashes.json` 全体も検証します。package集合、正規の相対path、hash形式を確認し、重複keyを拒否します。保全v5のSHA256を `34efbd109b4b2d3ce72bc327fe3972e35cf23907fe84aa67e32e8e59b9eab88e` に固定します。P1の書込先は展開package内の固定module pathだけとし、途中／末尾のsymlinkと、P1書込先のhardlinkを拒否します。hash・ELF・modinfo・梱包には同じ読み取り済みP1 bytesを使います。[S2検証](S2_VALIDATION.json)を参照してください。この版は梱包helperの修正で、配布v6 APKとS1実機記録は不変です。新しい実機受入結果は追加していません。

下のmodule pathはbuild-kernel.shの出力です。P1 hashは配布native buildの値で、新規buildではdebug path/toolchain等により変わり得るため、自分の検証済み出力から測定したhashを渡してください。正確な入力は[S1検証](S1_VALIDATION.json)、RAM実機結果は[低fps修正](LOW_FPS_FIX.ja.md)に記録しています。

```sh
python3 "$kit/scripts/package-low-fps-fix.py" \
 --accepted-repository /absolute/path/to/extracted-fresh-v5 \
 --p1-module "$work/kernel/modules/mt8183-p1-public/mt8183_p1.ko" \
 --p1-sha256 81bfb2020f3e5f8a0a6f3a1435ba1b751db06b439db3620db91e28d98e375d41 \
 --output "$work/s1-packages" \
 --sign-key /private/local-signing-key.rsa \
 --public-key /public/pmos@local-6ab3c683.rsa.pub
```
