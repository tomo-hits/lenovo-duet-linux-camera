/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT8183_CAMERA_RAW_H
#define MT8183_CAMERA_RAW_H
#ifdef DCV_RAW_HOST
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
#else
#include <linux/types.h>
#endif
#define DCV_RAW_WIDTH 1632U
#define DCV_RAW_HEIGHT 1224U
#define DCV_RAW_INPUT_BYTES (DCV_RAW_WIDTH * DCV_RAW_HEIGHT * 5U / 4U)
#define DCV_RAW_OUTPUT_BYTES (DCV_RAW_WIDTH * DCV_RAW_HEIGHT * 2U)
/* Input/output are disjoint CPU buffers. Preserve sample values and order;
 * only normalize MTISP continuous LE10 into V4L2 unpacked LE16 RAW10. */
int dcv_raw10_unpack(const u8 *input, size_t input_bytes,
		     u8 *output, size_t output_bytes);
#endif
