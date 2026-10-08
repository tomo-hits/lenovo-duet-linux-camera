# 外部USB起動実験の準備

[English](EXTERNAL_BOOT.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->

実機実験を再現する開発者向けの環境準備記録です。利用者向けの更新セット導入では実行しません。固定版pmbootstrapとpmaportsの取得・設定済み環境が前提で、空のホストからの自動セットアップではありません。**対応USBが起動済みなら、検証済みkitの`install.sh --external-usb`でファイル・パッケージを更新し、カメラ更新のたびにOSイメージを作り直す必要はありません。** USB起動、canonical DT・kernel・firmwareの適合、eMMC読み取り専用保護を確認済みです。過去modules 0.2.5候補 / meta 0.2.9は、P1のRAM専用チェックによる起動拒否に、保護検査付きの外部USB有効化で対応します。nativeビルド・梱包検査、実USB適用、コールドブート、自動有効化、前後2台の列挙に成功しました。前後撮影と新プロセスでの再利用も成功しました。復元と可聴音声の確認状況は[USB_ACCEPTANCE](USB_ACCEPTANCE.json)に記録します。

## 入力を一組に固定する

SKU176、postmarketOS v26.06 / Alpine v3.24、Phosh/systemd、音声PipeWire、Wi-Fi wpa_supplicantを使います。カーネルは正確な6.18.28-mt81のビルドです。`prepare-external-rootfs.py`は非圧縮Imageと展開後のSCP firmwareを`kernel-abi.json`のハッシュで検査します。配布側の`scp.img.zst`にも対応するため、rootfsに`zstd`を入れます。同じ版名でもハッシュの違うカーネルは拒否します。

準備に使ったpmbootstrapは3.11.1、commit `edb3097c7307216b088478b7c424ee07d636f41b`、pmaportsは `368093c7a882637ee00d32932fb0dafd24cfc4d4`です。配布リポジトリは更新され得るため入力を保全し、ハッシュ不一致なら中止します。カーネルとfirmwareはディストリビューションから取得する入力で、プロジェクトの同梱物ではありません。ドライバーの出典・ライセンス・原著作者は[OSS](OSS.ja.md)に記載しています。

## 配布rootfsを用意する

native ARM64 Linux上で、独立したpmbootstrap作業先と、変更していない固定版pmbootstrapソースを使います。`google-kukui`、上記OS/UI/providers、ユーザー`camera`、SSH鍵の取り込み無効、boot 512 MiB、extra space 2048 MiBを設定します。Python 3、OpenSSL、kmod、util-linux、zstd、Snapshot、libcamera-toolsを含めます。復旧用メディアは別に保全します。

次のラッパーを手元のターミナルで実行します。パスワードはrootfsの`passwd`へ直接入力し、上流の一時的な平文パスワードファイルを使いません。パスワード引数も受け付けません。この指定ではrootをロックし、sshdを無効にします。OSの通常のパスワードハッシュは保存されます。パスワードをコマンド引数・環境変数・スクリプトへ保存しないでください。

```sh
kit=/absolute/path/to/lenovo-duet-linux-camera/developer
pmb_tree=/absolute/path/to/pmbootstrap
work=/absolute/path/to/new-pmbootstrap-work
config=/absolute/path/to/pmbootstrap.cfg
python3 "$kit/scripts/pmbootstrap-install-direct-passwd.py" \
 --pmbootstrap-tree "$pmb_tree" -- -c "$config" -w "$work" install \
 --no-sshd --no-local-pkgs --no-recommends
```

ラッパーはイメージ/rootfsの導入だけを許可し、実ディスク、rsync、flasherモードを拒否します。任意の`--prepare-locked-account`は既にロック済みのアカウントを要求し、ログイン未設定のまま準備します。このイメージは利用前に、操作者が直接パスワードを設定する必要があります。自動準備の確認はこのモードで実施し、ログイン成功を確認したものではありません。

## 新しいイメージへ起動構成を組み込む

完成したイメージを調べる前に、固定版ツールでpmbootstrapのchrootをshutdownします。`pmbootstrap chroot --image`は使いません。このイメージはChromeOS kernel/boot/rootがp1/p2/p3にあり、同コマンドのboot/root p1/p2という前提に合いません。以下の検査は通常ファイルのイメージだけを読み取り専用で接続し、ビルダーの開始前に解除します。作業先のイメージの場所を確認してください。

```sh
stock="$work/chroot_native/home/pmos/rootfs/google-kukui.img"
test -f "$stock" && test ! -L "$stock" || exit 1
loop=$(sudo losetup --find --show --read-only --partscan "$stock")
root_uuid=$(sudo blkid -s UUID -o value "${loop}p3")
boot_uuid=$(sudo blkid -s UUID -o value "${loop}p2")
sudo losetup -d "$loop"
python3 "$kit/packaging/postmarketos/prepare-external-rootfs.py" \
 --rootfs "$work/chroot_rootfs_google-kukui" \
 --root-uuid "$root_uuid" --boot-uuid "$boot_uuid" \
 --output /absolute/path/to/new-overlay
sudo python3 "$kit/scripts/apply-external-overlay-to-image.py" \
 --image "$stock" --prepared /absolute/path/to/new-overlay \
 --output /absolute/path/to/new-camera.img
```

overlayにはcanonical camera DT、明示的なroot/boot UUID、初期initramfsの保護処理、カメラaliasのblacklist、systemdの有効化unitを含めます。ビルダーは元イメージを新規ファイルへコピーし、そのコピーだけをmountしてmkinitfs/FITを再生成します。カーネル署名とpartition容量を検査し、unmount後にファイルシステムを検査します。実デバイスをイメージ入力として受け付けません。比較用の元イメージと、復旧用のカメラ導入前の保護済みイメージを保全します。

保護処理はUUIDの一致が各1個で、両partitionが同じUSBにあり、検出したeMMC本体/partition/boot領域のread-onlyフラグを確認してからrootのmountを許可します。不一致なら起動を止めます。隔離fixtureと初回Duet起動で確認済みです。eMMCの内容、firmware、GBBを変更する処理ではありません。

## USBを識別してから書き込み・実機確認する

この補助はUSBの選定・消去を行いません。書き込む前に機種、容量、シリアル、mount状態を記録し、全partitionが対象の試験媒体にあることと、必要な内容を別媒体へ保存したことを確認します。実行直前にも再確認します。復旧用USBと内蔵eMMCは実験対象にしません。カメラ導入前のイメージは別ストレージに保全します。ログイン設定後はOSの私的な認証データを含むので公開しません。

USB起動後にkernel/DT、rootがUSBであること、eMMCのread-onlyフラグを確認し、検証済みの完全なパッケージ集合と外部USBモードで[INSTALL](../../docs/INSTALL.ja.md)を実施します。有効化serviceはカメラパッケージ未導入ならskipします。導入後は`duet-camera-activate --external-usb`を使い、root/bootがUSB上にあり、内蔵eMMCが読み取り専用であることを検査してからP1の外部USB動作を許可します。前後の写真をローカルへ保存し、Snapshotを閉じて再利用、SCPのoffline復帰、再起動後の再利用を確認します。通常の音声も確認します。認識、画像取得、正常な画質は別の到達点です。

ファイル・パッケージの復元はINSTALLを使います。過去modules 0.2.5では、実USBで全838パッケージ、world、保存した設定が元の状態に一致しました。復元後の再起動は未確認です。パッケージで戻せない場合は、同じ検証済み試験USBへカメラ導入前の保護済みイメージ全体を戻す経路を残します。この全イメージ復旧は未検証です。meta-packageの削除だけではlibcamera/PipeWireは戻りません。内蔵のboot設定やfirmwareは変更しないでください。

initramfsが2段構成の場合、上流処理が明示boot UUIDのpartitionを先に読み取り専用でmountして追加initramfsを読みます。保護hookはその後、rootや書き込み可能なbootのmountより前に実行されます。最初のbootの読み取りより前に実行される処理ではありません。

## 初回USB起動で確認した不足（2026-10-06）

USB上のroot/boot、canonicalカメラ構成、正確なkernel/FW、eMMC本体・全partition・boot領域のread-onlyを実機で確認しました。この初回起動時点ではカメラ未導入でした。標準のcodec/MDPドライバーとremoteprocの自動起動によりSCPがrunningとなり、導入前のoffline条件を満たしませんでした。

専用外部OS向けの修正候補は、codec/MDP/JPEGとSCPの自動読み込みを遮断し、デスクトップの開始前にSCP providerだけを明示的に読み込みます。固定カーネルの登録時に生じる自動起動参照1個だけを1回の通常停止で返し、offlineを確認します。既にproviderが読み込まれている起動、競合module/holder、2回目の試行、停止後もrunningの場合は拒否します。参照を繰り返し減らしたり、ドライバーを強制的に外したりしません。これによりハードウェアcodec/MDP/JPEGは専用テストOSで利用不可になります。

SCPの修正は次のUSB起動で成功しました。その後、旧modules 0.2.4-r1候補を適用してruntime206ファイルを照合しましたが、再起動時にP1の既存安全検査がRAM上のrootだけを許可するため、読み込みを拒否しました。この候補を復元し、元の全838パッケージ・world・設定へ戻したことを確認しました。

過去modules 0.2.5候補-r0では、既定で無効の`external_usb`パラメーターを追加しました。有効化処理が準備済みUSBのroot/bootと内蔵eMMCの読み取り専用状態を検査してから許可します。カーネルや起動イメージを新しく作らず、モジュールの再ビルドと再梱包で対応しました。[P1_USB_FIX](P1_USB_FIX.json)を参照してください。保全した導入前イメージは不変です。新候補の実USB適用、コールドブート、保護検査付き自動有効化、前後2台の列挙は成功し、再起動後もruntime206ファイルを照合しました。その後、SCP自動復旧設定も修正し、前後撮影、新プロセスでの再利用、元の838パッケージ・world・設定への復元が成功しました。復元後の起動と可聴音声は未確認です。[USB_ACCEPTANCE](USB_ACCEPTANCE.json)と[SCP_RECOVERY_FIX](SCP_RECOVERY_FIX.json)を参照してください。
