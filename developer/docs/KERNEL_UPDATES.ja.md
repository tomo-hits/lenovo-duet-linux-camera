# カーネル更新と再ビルド

[English](KERNEL_UPDATES.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->

**利用者は、使っているOS・カーネルに対応した更新セットを適用し、再起動します。** コマンドと現時点の検証状況は[INSTALL](../../docs/INSTALL.ja.md)を参照してください。再ビルドは配布側の作業で、対応済みのセットを導入する利用者には不要です。

現在の対象は、**6.18.28-mt81 / ARM64 / SKU176** の特定ビルドと、それに対応するDT・SCPファームウェアです。新しいカーネル用の更新セットが対応するまでは、動作中の外部環境を対応済みカーネルのまま維持します。カーネル名が同じでも、配布側の再ビルドで互換性が変わる場合があります。OS更新後の自動再ビルドには対応していません。

OS・カーネル更新を試す前に、動作中の**外部環境**を起動できる形で保全してください。カーネル、DTB、ファームウェア、カメラパッケージ、設定を含めます。`/etc/apk/world` だけでは足りません。更新後に動かなくなったら、保全した環境を起動し、[手動導入の適合検査](MANUAL_INSTALL.ja.md)と撮影確認を行います。カメラ更新のファイル・パッケージ復元だけでは、別途行ったOS・カーネル更新を元に戻せません。

## 配布側の作業

**対象カーネルの完全ビルド → 5つのカメラモジュール → 対応する梱包とABI記録 → 外部USBで実機試験**の順に進めます。これは今後のカーネルに移植するための手順で、既に対応済みという意味ではありません。対応済み6.18.28の同じビルドを再現する場合は[BUILD](BUILD.ja.md)を使います。

### 1. 対象カーネルのビルドを完了する

対象ディストリビューションのカーネルソース、パッチ、設定、ビルド手順を使います。対応するARM64ネイティブのClang/LLDでカーネルとそのモジュール全体をビルドし、次を保持してください。

- `.config`、生成済みヘッダー、`include/config/kernel.release`
- 同じ完全ビルドから生成した `vmlinux` と正規の `Module.symvers`
- ドライバーが必要とするmedia、videobuf2、remoteproc、MediaTek SCPの構成要素

下の補助スクリプトは、aarch64ネイティブLinux、KCFI有効、permissive CFI無効を要求します。Clangの識別情報とLLDの版も、カーネルのビルド記録と一致する必要があります。`make`、`clang`、`ld.lld`、`llvm-readelf`、`sha256sum`、kmod版の `modinfo` を使います。カーネルやツールチェーンの選定・取得は行いません。

ヘッダーパッケージや `modules_prepare` だけでは不十分です。`modules_prepare` は `Module.symvers` を生成しません。[Linux公式の外部モジュールビルド資料](https://docs.kernel.org/kbuild/modules.html)も参照してください。準備済み6.18.28 SDKは、`Module.symvers` を使わず公開シンボルを監査する別の方法を採用しています。このSDKにはBUILDを使い、古い `Module.symvers` を新カーネルへコピーして代用しないでください。

### 2. 5つのカメラモジュールをビルドする

パスを設定し、`YOUR_TARGET_KERNEL_RELEASE` を、起動予定のカーネル名に接尾辞も含めて置き換えます。対応するARM64ネイティブ環境で一般ユーザーとして実行してください。出力先は未作成のディレクトリを指定します。

```sh
repo=/absolute/path/to/lenovo-duet-linux-camera
kit="$repo/developer"
kernel_build=/absolute/path/to/complete-kernel-build
target_release=YOUR_TARGET_KERNEL_RELEASE
work=/absolute/path/to/new-rebuild-root
sh "$kit/scripts/build-modules-from-tree.sh" \
 "$kernel_build" "$target_release" "$work/kernel/modules" 3
```

補助スクリプトはモジュールのソースを出力先へコピーし、OV8856、OV02A10、DW9768、SENINF、P1をKbuildでビルドします。`build-inputs.txt`、`module-sha256.txt`、各モジュールの `build.log` と `modinfo.txt` を記録します。モジュールの導入や読み込みは行いません。

カーネルAPI、公開シンボル、コンパイラーの検査で失敗したら、対象のソースを移植し、新しい出力先で再ビルドします。modpostの失敗の無視、KCFIの無効化、強制ロードは行いません。ビルド成功と `vermagic` の一致だけでは、実機での動作確認にはなりません。

### 3. 対応する更新セットを梱包する

**現行の梱包・有効化処理は6.18.28-mt81固定です。別の対象を梱包する前に手動修正が必要です。** 新カーネル用の候補を、変更していない `package-fresh.py` へそのまま渡さないでください。

| ファイル・処理 | 新しい対象に合わせて確認する内容 |
| --- | --- |
| `scripts/package-fresh.py` | モジュールの導入先、パッケージ版、依存する版の指定 |
| `scripts/apply-update.py` | `PINS`のパッケージ版。自作版の鍵は独立確認した指紋を `--expected-key-sha256` で指定 |
| `packaging/postmarketos/kernel-abi.json` | カーネル名、実際のkernel notes/Imageのhash、検証したSCPファームウェア、機種 |
| `packaging/postmarketos/duet-camera-activate.py` | `depmod` に渡すカーネル名。`--external-usb`のUSB/eMMC検査とP1の既定で無効な`external_usb`許可を維持 |
| `packaging/postmarketos/camera-nodes.json` とDT補助スクリプト | 新カーネルのカメラ接続構成と定義 |
| 外部OSの起動・更新用スクリプト | モジュール名、SCP初期化、適合確認 |
| `configs/kernel/` と `docs/SOURCE_INPUTS.json` | ソースの版、配布側のパッチ・設定、ツールチェーン、入力の出典 |

hashは新しい実ビルドと試験起動から取得し、従来の対象の記録も残します。固定入力を使うBUILDの方法を拡張する場合は、入力検証、hash一覧、公開シンボルの監査処理も見直します。期待値だけの差し替えでは済みません。ユーザー空間のバイナリを再利用するのは、mediaインターフェースと配布側の依存関係が引き続き適合する場合に限り、それ以外はBUILDで再ビルドします。

梱包処理を修正して入力を確認したら、BUILDの署名付き梱包手順で新しい候補セットを作成します。検証用公開鍵、パッケージhash、対応するモジュール・ユーザー空間のソース、ビルド手順、著作権表示を揃えます。署名秘密鍵はローカルに保持します。元のSPDX・著作権表示とライセンスを維持し、ソースの変更に合わせて[PROVENANCE](PROVENANCE.ja.md)と[MODIFICATIONS.json](MODIFICATIONS.json)を更新してください。バイナリと併せて配布する対応ソースは[OSS](OSS.ja.md)を参照してください。

### 4. 実機で確認して対応版として配布する

別の外部USB実験環境を使い、内蔵eMMCのOS・ブート領域・ファームウェアは変更しません。まず新しい対象カーネルを起動し、DT・ファームウェアと適合記録を確認します。次に候補の更新セットを適用して再起動し、以下を確認します。

1. 対象カーネルと5つのカメラモジュールが動作し、前後のセンサーを認識する。
2. 通常のカメラアプリで前後ライブ、撮影、切替、通常終了、再利用ができ、SCPが所定の待機状態へ戻る。
3. 通常の音声が使え、もう一度再起動してもカメラを利用できる。
4. 更新セットの復元手順で、導入前の状態へ戻せる。

**ビルド成功・センサー認識・画像取得・正常な画質は別々に記録**し、カメラ画像はローカルに保持します。詳しい検査と既存の制限は[TESTING](TESTING.ja.md)を参照してください。これらが通ってから、対応カーネルのビルド・OS、更新セットのhash、ソースの版、試験結果を揃えて配布します。

この文書で新カーネルのビルドや実機動作を検証したわけではありません。補助スクリプトのホスト試験で確認しているのは、コマンド処理と不適切な入力の拒否です。
