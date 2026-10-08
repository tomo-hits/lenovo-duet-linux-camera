// SPDX-License-Identifier: GPL-2.0-only
/* Independent bit-stream oracle on synthetic data only. */
#include "camera_raw.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_sample(u8 *p, size_t index, unsigned int value)
{
	unsigned int bit;
	for (bit = 0; bit < 10; bit++) {
		size_t position = index * 10 + bit;
		u8 mask = (u8)(1U << (position % 8));
		p[position / 8] = (p[position / 8] & ~mask) |
			((value & (1U << bit)) ? mask : 0);
	}
}
static unsigned int get_sample(const u8 *p, size_t index)
{
	unsigned int bit, value = 0;
	for (bit = 0; bit < 10; bit++) {
		size_t position = index * 10 + bit;
		value |= ((p[position / 8] >> (position % 8)) & 1U) << bit;
	}
	return value;
}
int main(void)
{
	const size_t pixels = DCV_RAW_WIDTH * DCV_RAW_HEIGHT;
	u8 *raw = calloc(1, DCV_RAW_INPUT_BYTES);
	u8 *storage = malloc(DCV_RAW_OUTPUT_BYTES + 32), *out = storage + 16;
	size_t i;
	unsigned int pass, seed = 0x51a078;
	assert(raw && storage);
	for (pass = 0; pass < 3; pass++) {
		for (i = 0; i < pixels; i++) {
			unsigned int value;
			if (!pass) value = (unsigned int)(i / 4) % 1024;
			else if (pass == 1) value = i % 2 ? 1023 : 0;
			else { seed = seed * 1664525U + 1013904223U; value = seed >> 22; }
			put_sample(raw, i, value);
		}
		memset(storage, 0xa5, DCV_RAW_OUTPUT_BYTES + 32);
		assert(!dcv_raw10_unpack(raw, DCV_RAW_INPUT_BYTES, out, DCV_RAW_OUTPUT_BYTES));
		for (i = 0; i < pixels; i++) {
			assert((out[2*i] | ((unsigned int)out[2*i+1] << 8)) == get_sample(raw, i));
			assert(!(out[2*i+1] & 0xfc));
		}
		for (i = 0; i < 16; i++) {
			assert(storage[i] == 0xa5);
			assert(storage[DCV_RAW_OUTPUT_BYTES + 16 + i] == 0xa5);
		}
	}
	memset(out, 0x5c, DCV_RAW_OUTPUT_BYTES);
	assert(dcv_raw10_unpack(NULL, DCV_RAW_INPUT_BYTES, out, DCV_RAW_OUTPUT_BYTES) == -EINVAL);
	assert(dcv_raw10_unpack(raw, DCV_RAW_INPUT_BYTES, NULL, DCV_RAW_OUTPUT_BYTES) == -EINVAL);
	assert(dcv_raw10_unpack(raw, DCV_RAW_INPUT_BYTES-1, out, DCV_RAW_OUTPUT_BYTES) == -EINVAL);
	assert(dcv_raw10_unpack(raw, DCV_RAW_INPUT_BYTES+1, out, DCV_RAW_OUTPUT_BYTES) == -EINVAL);
	assert(dcv_raw10_unpack(raw, DCV_RAW_INPUT_BYTES, out, DCV_RAW_OUTPUT_BYTES-1) == -EINVAL);
	for (i = 0; i < DCV_RAW_OUTPUT_BYTES; i++) assert(out[i] == 0x5c);
	assert(!dcv_raw10_unpack(raw, DCV_RAW_INPUT_BYTES, out, DCV_RAW_OUTPUT_BYTES+16));
	for (i = 0; i < 16; i++) assert(storage[DCV_RAW_OUTPUT_BYTES + 16 + i] == 0xa5);
	free(raw);free(storage);
	puts("PASS: 5760000 synthetic samples, all 1024 codes at all four packed positions, row/end boundaries, upper bits, guards, invalid sizes");
	return 0;
}
