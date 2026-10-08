# 内蔵eMMCの起動統合と復旧

[English](INTERNAL_EMMC.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->


対応環境での導入・復元は[利用者向け手順](../../docs/INSTALL.ja.md)に従ってください。ツールは対応するOS・カーネルを維持し、カメラ用デバイスツリーを組み込みます。任意の内蔵OS構成には対応しません。

## 適合検査と保存対象

`boot-update.py`はroot/bootが同じeMMCにあること、CID・filesystem UUID・区画位置、kernel notesと展開Image、FIT内のkernel/initramfs/DT、SCP firmwareを検査します。元の32 MiB kernel slot、boot3ファイル、fstab、隔離設定2ファイルをroot専用領域に保存します。候補の署名、非対象DT26個、起動引数の保持を確認してから書込み、全kernel slotの読戻しとファイルの内容・属性を検証します。

パッケージ更新は別に署名を検証し、元の正確なAPK・world・設定を保存してoffline逆transactionを確認します。user mediaの自動起動は選択したデスクトップユーザーに限定します。firmware・GPT・hardware boot0/boot1は変更しません。単一slot書込みは電源断に対してatomicではなく、コマンドによる復元と起動不能時の復旧は別の経路です。

## 復旧の確認

更新セットとboot・パッケージbackupを保管してください。復元は同じ媒体と記録対象の一致を要求し、外部変更があれば停止します。アプリを閉じ、正常なSTOP診断を確認します。STOP失敗の保護を迂回したり、ドライバーを強制解除したりしないでください。起動不能や照合不能の場合は、別途保全したOS復旧backupを使います。

通常復元と再起動後に、パッケージ・world、boot・設定の内容と属性、mount条件、通常media動作を照合します。記録した対象の復元であり、全filesystemのbyte一致を示しません。

## 版ごとの実機結果

- 2026-10-06のmodules 0.2.6／meta 0.2.10は、起動構成を別途準備した実験です。パッケージと別工程のboot・設定復元、復元後の通常起動・ログインを確認しました。当時の更新ツールはbootを準備・復元しません。[INTERNAL_ACCEPTANCE](INTERNAL_ACCEPTANCE.json)。
- v1.0.0は起動準備・復元を統合し、限定した撮影・sleep・記録対象の復元を確認しました。[PUBLIC_ACCEPTANCE](PUBLIC_ACCEPTANCE.json)。
- v1.0.1の復旧保護は、後日の導入・撮影・音声・sleep・復元試験でも確認しました。後gamma比較は同じELFを使い、新APKセットの導入・復元は未試験です。[2026-10-08の結果](ACCEPTANCE_2026-10-08.json)。
