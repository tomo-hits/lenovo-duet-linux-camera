/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
/* Pure codec; no permission to publish addresses or access hardware. */
#ifndef MT8183_P1_STREAM_CODEC_H
#define MT8183_P1_STREAM_CODEC_H

#ifdef MT8183_P1_STREAM_HOST_TEST
#include <stdbool.h>
#include <stddef.h>
#endif
#include <linux/types.h>

#define MT8183_P1_STREAM_CMD_BYTES 129U
#define MT8183_P1_STREAM_FRAME_BYTES 60U
#define MT8183_P1_STREAM_RX_MAX 288U
#define MT8183_P1_STREAM_BUFFERS 7U
#define MT8183_P1_STREAM_COMPOSER_BYTES 0x200000U
#define MT8183_P1_STREAM_IPI_CMD 10U
#define MT8183_P1_STREAM_IPI_FRAME 11U

/* Explicit acknowledgement of an UNVERIFIED FW profile, not FW approval. */
#define MT8183_P1_PROFILE_UNVERIFIED_META_ZERO 1U
struct mt8183_p1_stream_profile {
	u32 width;
	u32 height;
	u32 bayer_id; /* B=0, GB=1, GR=2, R=3; read sensor ACTIVE format. */
	u32 flags;
};

struct mt8183_p1_stream_layout {
	u32 stride;
	u32 image_bytes;
};

/* Size describes the whole owned mapping, not merely the wire address. */
struct mt8183_p1_stream_span {
	u64 iova;
	u64 scp_addr;
	u64 bytes;
};

enum mt8183_p1_stream_rx_kind {
	MT8183_P1_STREAM_RX_EMPTY,
	MT8183_P1_STREAM_RX_RAW,
	MT8183_P1_STREAM_RX_SHORT_ACK,
	MT8183_P1_STREAM_RX_ACK_OTHER,
	MT8183_P1_STREAM_RX_FRAME_ACK,
};

struct mt8183_p1_stream_rx {
	u32 ipi_id;
	u32 len;
	enum mt8183_p1_stream_rx_kind kind;
	bool has_command;
	bool has_ack_command;
	u8 command;
	u8 ack_command;
	u32 sequence;
	u8 bytes[MT8183_P1_STREAM_RX_MAX];
};

/* Checked arithmetic follows the old raw10 stride rule; no FW claim. */
int mt8183_p1_stream_raw10_layout(u32 width, u32 height,
				struct mt8183_p1_stream_layout *layout);

/* Initial profile is exactly 1632x1224, raw10 packed, IMGO only, no meta.
 * All encoders leave output untouched on failure and write exactly the
 * indicated wire size on success. Capacity may be larger. Aliasing is safe.
 */
int mt8183_p1_stream_encode_config(void *out, size_t capacity,
				const struct mt8183_p1_stream_profile *profile);
int mt8183_p1_stream_encode_meta(void *out, size_t capacity,
			      const struct mt8183_p1_stream_profile *profile);
int mt8183_p1_stream_encode_stream(void *out, size_t capacity, unsigned int on);
int mt8183_p1_stream_encode_frame(void *out, size_t capacity,
			       const struct mt8183_p1_stream_profile *profile,
			       u32 sequence,
			       const struct mt8183_p1_stream_span buffers[MT8183_P1_STREAM_BUFFERS]);

/* Copy first, never keep SCP share-buffer pointers. Both old host channels
 * accept FRAME_ACK-shaped payloads. This is classification, not transport,
 * INIT/CONFIG/DEINIT success, DMA completion or buffer-release permission.
 * Invalid input zeros a non-NULL output. packet/output overlap is supported.
 */
int mt8183_p1_stream_observe(u32 ipi_id, const void *packet, size_t len,
			   struct mt8183_p1_stream_rx *out);

#endif
