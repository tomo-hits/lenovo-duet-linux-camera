/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT8183_P1_LEGACY_H
#define MT8183_P1_LEGACY_H
/* Deprecated development identifiers. Existing DT/boot ABI compatibility
 * only; new descriptions must use the hardware names in the main driver. */
#define MT8183_P1_LEGACY_COMPAT "codex,mt8183-p1-integrated"
#define MT8183_P1_LEGACY_PATH "/p1-integrated-test"
#define MT8183_P1_LEGACY_NAME "p1-integrated-test"
#define MT8183_P1_LEGACY_RAM_MARKER "duet_camera_ram=baseline"
static const char * const mt8183_p1_legacy_exclusive[] = {
 "codex,mt8183-p1-composer", "codex,mt8183-p1-dma-probe",
 "codex,mt8183-p1-graph", "codex,mt8183-p1-resources",
};
#endif
