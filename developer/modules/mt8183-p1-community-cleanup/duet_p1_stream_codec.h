/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
/* Pure codec; no permission to publish addresses or access hardware. */
#ifndef DUET_P1_STREAM_CODEC_H
#define DUET_P1_STREAM_CODEC_H

#ifdef DUET_P1_STREAM_HOST_TEST
#include <stdbool.h>
#include <stddef.h>
#endif
#include <linux/types.h>

#define DUET_P1_STREAM_CMD_BYTES 129U
#define DUET_P1_STREAM_FRAME_BYTES 60U
#define DUET_P1_STREAM_RX_MAX 288U
#define DUET_P1_STREAM_BUFFERS 7U
#define DUET_P1_STREAM_COMPOSER_BYTES 0x200000U
#define DUET_P1_STREAM_IPI_CMD 10U
#define DUET_P1_STREAM_IPI_FRAME 11U

/* Explicit acknowledgement of an UNVERIFIED FW profile, not FW approval. */
#define DUET_P1_PROFILE_UNVERIFIED_META_ZERO 1U
struct duet_p1_stream_profile {
	u32 width;
	u32 height;
	u32 bayer_id; /* B=0, GB=1, GR=2, R=3; read sensor ACTIVE format. */
	u32 flags;
};

struct duet_p1_stream_layout {
	u32 stride;
	u32 image_bytes;
};

/* Size describes the whole owned mapping, not merely the wire address. */
struct duet_p1_stream_span {
	u64 iova;
	u64 scp_addr;
	u64 bytes;
};

enum duet_p1_stream_rx_kind {
	DUET_P1_STREAM_RX_EMPTY,
	DUET_P1_STREAM_RX_RAW,
	DUET_P1_STREAM_RX_SHORT_ACK,
	DUET_P1_STREAM_RX_ACK_OTHER,
	DUET_P1_STREAM_RX_FRAME_ACK,
};

struct duet_p1_stream_rx {
	u32 ipi_id;
	u32 len;
	enum duet_p1_stream_rx_kind kind;
	bool has_command;
	bool has_ack_command;
	u8 command;
	u8 ack_command;
	u32 sequence;
	u8 bytes[DUET_P1_STREAM_RX_MAX];
};

/* Checked arithmetic follows the old raw10 stride rule; no FW claim. */
int duet_p1_stream_raw10_layout(u32 width, u32 height,
				struct duet_p1_stream_layout *layout);

/* Initial profile is exactly 1632x1224, raw10 packed, IMGO only, no meta.
 * All encoders leave output untouched on failure and write exactly the
 * indicated wire size on success. Capacity may be larger. Aliasing is safe.
 */
int duet_p1_stream_encode_config(void *out, size_t capacity,
				const struct duet_p1_stream_profile *profile);
int duet_p1_stream_encode_meta(void *out, size_t capacity,
			      const struct duet_p1_stream_profile *profile);
int duet_p1_stream_encode_stream(void *out, size_t capacity, unsigned int on);
int duet_p1_stream_encode_frame(void *out, size_t capacity,
			       const struct duet_p1_stream_profile *profile,
			       u32 sequence,
			       const struct duet_p1_stream_span buffers[DUET_P1_STREAM_BUFFERS]);

/* Copy first, never keep SCP share-buffer pointers. Both old host channels
 * accept FRAME_ACK-shaped payloads. This is classification, not transport,
 * INIT/CONFIG/DEINIT success, DMA completion or buffer-release permission.
 * Invalid input zeros a non-NULL output. packet/output overlap is supported.
 */
int duet_p1_stream_observe(u32 ipi_id, const void *packet, size_t len,
			   struct duet_p1_stream_rx *out);

#endif
