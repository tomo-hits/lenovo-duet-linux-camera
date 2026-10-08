# postmarketOS統合helper

[English](README.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->


ソースbuildと署名APKは[BUILD](../../docs/BUILD.ja.md)、開発時の個別操作は[手動導入](../../docs/MANUAL_INSTALL.ja.md)を参照してください。通常の導入・復元は[利用者手順](../../../docs/INSTALL.ja.md)です。

helperは機種・kernel・firmwareの適合、camera graph全体と旧binding、boot asset準備、対応module有効化を扱います。入口ごとの範囲を確認してください。低水準DT生成はboot媒体を書きませんが、対応するboot updaterは元内容を保存してから検証済みkernel slotへ書込みます。

過去modules 0.2.5／meta 0.2.9で外部USB canonical DT cold boot・有効化が成功しました。内蔵・tuningの結果は[TESTING](../../docs/TESTING.ja.md)に分けて記録し、旧USB結果を現行packageの受入と扱いません。
