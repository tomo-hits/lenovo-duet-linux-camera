# 構成

[English](ARCHITECTURE.md) | 日本語

<!-- SPDX-License-Identifier: CC0-1.0 -->

OV02A10（前面）、OV8856（背面）、DW9768 lens、MediaTek SENINF receiver、MT8183 P1 RAW captureを組み合わせます。標準sensor/lens controlがmedia graphへ入り、P1はV4L2/VB2 bufferと標準stream操作を公開します。RAW10変換とコピーはCPU処理です。libcamera Simple/SoftISPは共通AE/AWB/debayerと既存RPiControllerのAFを使います。PipeWireのlibcamera SPA nodeがportal経由で通常のデスクトップアプリへ接続します。直接libcameraを使うツールではリア高解像度を選べます。

P1/SENINFにはChromiumOS `527db0b5974bb70364fc692448a55fb37209fe23` に由来する固定MediaTek firmware/register protocolが残ります。完全なISPの代替ではありません。codecとSCPの同時所有は未確認です。

通常STREAMOFFにはinput停止、IRQ同期、SCP/IPI終了、CAM電源／所有権の明示的確認、workerの完了が必要です。確認に失敗するとDMAを保持し再利用を拒否します。正常Close後も停止済みDMAを再利用用に保持し、後のretirement/device removalが所有権を確認して解放します。動作中カメラのsleepは非対応なので、先にアプリを閉じます。

標準P1には独自sysfs画像／control interfaceや故障注入parameterはありません。任意のroot専用数値診断は `/sys/kernel/debug/mt8183_p1/<device>/` にあり、アプリはdebugfs無しで動きます。

`mt8183_p1_legacy.h` に旧DT/boot識別子と競合driverの除外をまとめています。新DT生成ヘルパーはgraph全体、実provider phandle、既存property/reservationの保持を検証します。出力はローカル提案で、上流承認bindingではありません。boot partitionの選択・書き込みは行いません。
