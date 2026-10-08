/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DCV_SOFTISP_H
#define DCV_SOFTISP_H
#ifdef DCV_ISP_HOST
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
#else
#include <linux/types.h>
#endif
int dcv_softisp(const u8 *,size_t,u8 *,size_t);
#endif
