# Internal eMMC boot integration and recovery

English | [日本語](INTERNAL_EMMC.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->


For supported installation and restoration, follow the [user guide](../../docs/INSTALL.md). The toolkit preserves the matched OS/kernel and integrates the camera device tree. It does not support arbitrary internal-OS layouts.

## Compatibility and saved state

`boot-update.py` checks that root and boot are on the same eMMC, along with CID, filesystem UUIDs, partition locations, kernel notes and unpacked Image, FIT kernel/initramfs/DT contents, and SCP firmware. It saves the original 32 MiB kernel slot, three boot files, fstab and two camera isolation files in root-only storage. Before writing, it checks the candidate signature, 26 unaffected DTs and the preserved command line. It reads back the entire kernel slot and verifies file contents and metadata.

The package updater separately checks signatures, saves exact original APKs/world/configuration and verifies the offline reverse transaction. User-media startup is limited to the selected desktop user. Firmware, GPT and hardware boot0/boot1 are unchanged. A single-slot write is not power-failure atomic; command restoration and recovery from an unbootable OS are different paths.

## Recovery checks

Retain the kit and boot/package backups. Restoration requires the same media and matching recorded targets and refuses external changes. Close camera applications and require clean STOP diagnostics. Do not bypass a failed STOP or force driver teardown. If the OS cannot boot or restoration checks cannot be satisfied, use the independently preserved OS recovery backup.

After ordinary restoration and reboot, verify the package/world state, saved boot/configuration contents and attributes, mount conditions and normal media operation. The claim covers recorded targets, not all filesystem bytes.

## Versioned hardware evidence

- The 2026-10-06 modules 0.2.6 / meta 0.2.10 experiment used separately prepared boot configuration. Package restoration and separate boot/configuration restoration passed, followed by normal boot/login. Its updater did not itself prepare or restore boot assets. [INTERNAL_ACCEPTANCE](INTERNAL_ACCEPTANCE.json).
- v1.0.0 added integrated boot preparation/restoration and passed scoped capture, sleep and target restoration. [PUBLIC_ACCEPTANCE](PUBLIC_ACCEPTANCE.json).
- v1.0.1 recovery safeguards passed the later installation, capture, audio, sleep and restoration run. The rear-gamma comparison reused its ELF binaries; installation/restoration of the newly repackaged APK set remain untested. [2026-10-08 results](ACCEPTANCE_2026-10-08.json).
