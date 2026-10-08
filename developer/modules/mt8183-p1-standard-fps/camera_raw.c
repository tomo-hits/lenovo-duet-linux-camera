/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
// SPDX-License-Identifier: GPL-2.0-only
/* MTISP packing layout from this repository's MIT duet-softisp reference.
 * See userspace/duet-softisp/LICENSE. No ISP, AE/AWB or calibration here. */
#include "camera_raw.h"
#ifdef DCV_RAW_HOST
#include <errno.h>
#else
#include <linux/errno.h>
#endif

int dcv_raw10_unpack(const u8 *input, size_t input_bytes,
		     u8 *output, size_t output_bytes)
{
	size_t i, o;

	if (!input || !output || (input_bytes != 2400000U && input_bytes != DCV_RAW_INPUT_BYTES && input_bytes != 9987840U) ||
	    output_bytes < input_bytes / 5U * 8U)
		return -EINVAL;

	for (i = 0, o = 0; i < input_bytes; i += 5, o += 8) {
		unsigned int v[4] = {
			input[i] | ((input[i + 1] & 3U) << 8),
			(input[i + 1] >> 2) | ((input[i + 2] & 15U) << 6),
			(input[i + 2] >> 4) | ((input[i + 3] & 63U) << 4),
			(input[i + 3] >> 6) | ((unsigned int)input[i + 4] << 2),
		};
		unsigned int j;

		for (j = 0; j < 4; j++) {
			output[o + 2 * j] = v[j] & 255U;
			output[o + 2 * j + 1] = v[j] >> 8;
		}
	}
	return 0;
}
