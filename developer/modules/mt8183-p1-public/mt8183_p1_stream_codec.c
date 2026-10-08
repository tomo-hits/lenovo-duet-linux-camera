/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
// SPDX-License-Identifier: GPL-2.0-only
/* Wire layouts: MediaTek (2019), GPL-2.0, ChromiumOS 527db0b5974b cam. */
#ifdef MT8183_P1_STREAM_HOST_TEST
#include <errno.h>
#include <string.h>
#else
#include <linux/errno.h>
#include <linux/stddef.h>
#include <linux/string.h>
#endif
#include "mt8183_p1_stream_codec.h"
#include "mtk_cam-ipi.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error Fixed MT8183 wire ABI requires little endian
#endif

#define ASSERT(e) _Static_assert(e, #e)
ASSERT(sizeof(struct mtk_isp_scp_p1_cmd) == MT8183_P1_STREAM_CMD_BYTES);
ASSERT(sizeof(struct p1_config_param) == 125);
ASSERT(sizeof(struct cfg_in_param) == 27);
ASSERT(sizeof(struct cfg_main_out_param) == 48);
ASSERT(sizeof(struct cfg_resize_out_param) == 46);
ASSERT(sizeof(struct dma_buffer) == 8);
ASSERT(4 + 7 * sizeof(struct dma_buffer) == MT8183_P1_STREAM_FRAME_BYTES);
ASSERT(offsetof(struct mtk_isp_scp_p1_cmd, config_param.cfg_in_param) == 1);
ASSERT(offsetof(struct mtk_isp_scp_p1_cmd, config_param.cfg_main_param) == 28);
ASSERT(offsetof(struct mtk_isp_scp_p1_cmd, config_param.cfg_resize_param) == 76);
ASSERT(offsetof(struct mtk_isp_scp_p1_cmd, config_param.enabled_dmas) == 122);
ASSERT(offsetof(struct mtk_isp_scp_p1_cmd, ack_info.frame_seq_no) == 2);
ASSERT(sizeof(struct isp_ack_info) + 1 == 6);
ASSERT(ISP_CMD_CONFIG == 1 && ISP_CMD_STREAM == 2 && ISP_CMD_ACK == 4 &&
       ISP_CMD_FRAME_ACK == 5 && ISP_CMD_CONFIG_META == 6);

static void put32(u8 *dst, u32 value)
{
	dst[0] = value;
	dst[1] = value >> 8;
	dst[2] = value >> 16;
	dst[3] = value >> 24;
}

static u32 get32(const u8 *src)
{
	return (u32)src[0] | (u32)src[1] << 8 |
	       (u32)src[2] << 16 | (u32)src[3] << 24;
}

int mt8183_p1_stream_raw10_layout(u32 width, u32 height,
				struct mt8183_p1_stream_layout *layout)
{
	u64 stride, bytes;

	if (!layout || !width || !height)
		return -EINVAL;
	/* Widen before multiply and check before narrowing. */
	stride = (((u64)width * 10 + 7) / 8 + 3) & ~3ULL;
	if (stride > 0xffffffffULL)
		return -ERANGE;
	bytes = stride * height;
	if (bytes > 0xffffffffULL)
		return -ERANGE;
	layout->stride = stride;
	layout->image_bytes = bytes;
	return 0;
}

static int profile_layout(const struct mt8183_p1_stream_profile *profile,
			  struct mt8183_p1_stream_layout *layout)
{
	if (!profile || profile->flags != MT8183_P1_PROFILE_UNVERIFIED_META_ZERO ||
	    profile->bayer_id > 3)
		return -EINVAL;
	/* Expand formats only with a separate profile and review. */
	if (!((profile->width == 1632 && profile->height == 1224) ||
	      (profile->width == 1600 && profile->height == 1200) ||
	      (profile->width == 3264 && profile->height == 2448)))
		return -EOPNOTSUPP;
	return mt8183_p1_stream_raw10_layout(profile->width, profile->height, layout);
}

int mt8183_p1_stream_encode_config(void *out, size_t capacity,
				const struct mt8183_p1_stream_profile *profile)
{
	struct mt8183_p1_stream_layout layout;
	u8 bytes[MT8183_P1_STREAM_CMD_BYTES] = { 0 };
	int ret;

	if (!out)
		return -EINVAL;
	if (capacity < sizeof(bytes))
		return -ENOSPC;
	ret = profile_layout(profile, &layout);
	if (ret)
		return ret;
	bytes[0] = ISP_CMD_CONFIG;
	bytes[1] = 1; /* continuous */
	bytes[3] = 1; /* one pixel mode */
	bytes[5] = profile->bayer_id;
	put32(bytes + 8, 0x2201); /* input Bayer10 */
	put32(bytes + 20, profile->width); /* input crop width/height */
	put32(bytes + 24, profile->height);
	bytes[29] = 1; /* pure_raw; main bypass remains 0 */
	bytes[30] = 1; /* packed */
	/* CONFIG output address remains zero; FRAME carries the real buffer. */
	put32(bytes + 39, profile->width);
	put32(bytes + 43, profile->height);
	put32(bytes + 47, layout.stride);
	put32(bytes + 51, layout.stride);
	put32(bytes + 63, profile->width); /* output crop */
	put32(bytes + 67, profile->height);
	bytes[71] = 10;
	put32(bytes + 72, 0x2201);
	bytes[76] = 1; /* resize bypass */
	put32(bytes + 122, 1); /* R_IMGO */
	memcpy(out, bytes, sizeof(bytes));
	return 0;
}

int mt8183_p1_stream_encode_meta(void *out, size_t capacity,
			      const struct mt8183_p1_stream_profile *profile)
{
	struct mt8183_p1_stream_layout layout;
	u8 bytes[MT8183_P1_STREAM_CMD_BYTES] = { 0 };
	int ret;

	if (!out)
		return -EINVAL;
	if (capacity < sizeof(bytes))
		return -ENOSPC;
	ret = profile_layout(profile, &layout);
	if (ret)
		return ret;
	bytes[0] = ISP_CMD_CONFIG_META;
	put32(bytes + 1, 1); /* Old CONFIG_META mask includes IMGO. */
	memcpy(out, bytes, sizeof(bytes));
	return 0;
}

int mt8183_p1_stream_encode_stream(void *out, size_t capacity, unsigned int on)
{
	u8 bytes[MT8183_P1_STREAM_CMD_BYTES] = { 0 };

	if (!out || on > 1)
		return -EINVAL;
	if (capacity < sizeof(bytes))
		return -ENOSPC;
	bytes[0] = ISP_CMD_STREAM;
	bytes[1] = on;
	memcpy(out, bytes, sizeof(bytes));
	return 0;
}

int mt8183_p1_stream_encode_frame(void *out, size_t capacity,
			       const struct mt8183_p1_stream_profile *profile,
			       u32 sequence,
			       const struct mt8183_p1_stream_span buffers[MT8183_P1_STREAM_BUFFERS])
{
	struct mt8183_p1_stream_layout layout;
	u8 bytes[MT8183_P1_STREAM_FRAME_BYTES] = { 0 };
	unsigned int i;
	int ret;

	if (!out || !buffers || !sequence)
		return -EINVAL;
	if (capacity < sizeof(bytes))
		return -ENOSPC;
	ret = profile_layout(profile, &layout);
	if (ret)
		return ret;
	for (i = 0; i < MT8183_P1_STREAM_BUFFERS; i++) {
		const struct mt8183_p1_stream_span *span = &buffers[i];

		if (i != 1) {
			if (span->iova || span->scp_addr || span->bytes)
				return -EINVAL;
			continue;
		}
		if (!span->iova || span->scp_addr || span->bytes < layout.image_bytes)
			return -EINVAL;
		/* Check complete owned range without overflowing a sum. */
		if (span->iova > 0xffffffffULL || span->bytes > 0x100000000ULL ||
		    span->bytes - 1 > 0xffffffffULL - span->iova)
			return -ERANGE;
		put32(bytes + 4 + i * 8, span->iova);
	}
	put32(bytes, sequence);
	memcpy(out, bytes, sizeof(bytes));
	return 0;
}

int mt8183_p1_stream_observe(u32 ipi_id, const void *packet, size_t len,
			   struct mt8183_p1_stream_rx *out)
{
	struct mt8183_p1_stream_rx rx = { 0 };

	if (!out)
		return -EINVAL;
	if ((ipi_id != MT8183_P1_STREAM_IPI_CMD && ipi_id != MT8183_P1_STREAM_IPI_FRAME) ||
	    len > MT8183_P1_STREAM_RX_MAX || (!packet && len)) {
		memset(out, 0, sizeof(*out));
		return -EINVAL;
	}
	rx.ipi_id = ipi_id;
	rx.len = len;
	if (len) {
		memcpy(rx.bytes, packet, len);
		rx.has_command = true;
		rx.command = rx.bytes[0];
		rx.kind = MT8183_P1_STREAM_RX_RAW;
		if (rx.command == ISP_CMD_ACK) {
			rx.kind = MT8183_P1_STREAM_RX_SHORT_ACK;
			if (len >= 2) {
				rx.has_ack_command = true;
				rx.ack_command = rx.bytes[1];
			}
			if (len >= 6) {
				rx.sequence = get32(rx.bytes + 2);
				rx.kind = rx.ack_command == ISP_CMD_FRAME_ACK ?
					MT8183_P1_STREAM_RX_FRAME_ACK : MT8183_P1_STREAM_RX_ACK_OTHER;
			}
		}
	}
	memcpy(out, &rx, sizeof(rx));
	return 0;
}
