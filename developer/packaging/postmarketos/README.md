# postmarketOS integration helpers

English | [日本語](README.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->


[BUILD](../../docs/BUILD.md) describes source builds and signed APK packaging. [Manual installation](../../docs/MANUAL_INSTALL.md) describes individual development operations; ordinary installation/restoration uses the [user guide](../../../docs/INSTALL.md).

The helpers validate hardware/kernel/firmware compatibility, integrate the complete camera graph and legacy bindings, prepare boot assets and activate matched modules. Inspect the chosen entry point's documented scope: the low-level DT generator does not write boot media, while the supported boot updater does write the verified kernel slot and saves its original contents first.

Historical external-USB canonical-DT cold boot and activation passed on modules 0.2.5 / meta 0.2.9. Current internal and tuning results are recorded separately in [TESTING](../../docs/TESTING.md); older USB evidence does not establish current package acceptance.
