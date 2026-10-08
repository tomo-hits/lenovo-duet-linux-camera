/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
/* Frame wire declaration derived from MediaTek Inc. (c) 2019 GPL-2.0 code. */
#ifndef MT8183_P1_ABI_H
#define MT8183_P1_ABI_H

#ifdef MT8183_P1_ABI_HOST_TEST
#include <stdbool.h>
#include <stddef.h>
#else
#include <linux/stddef.h>
#endif
#include "mtk_cam-ipi.h"

/* Original MT8183 wire ABI is native little endian; no big-endian port. */
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error The pinned MT8183 P1 wire ABI requires a little-endian compiler target
#endif

#define MT8183_P1_CMD_BYTES 129U
#define MT8183_P1_FRAME_BYTES 60U
#define MT8183_P1_ACK_MIN_BYTES 6U
#define MT8183_P1_IPI_CMD 10U
#define MT8183_P1_IPI_FRAME 11U
#define MT8183_P1_RX_MAX_BYTES 288U
#define MT8183_P1_WRAPPER_MAX_BYTES 140U
#define MT8183_P1_COMPOSER_BYTES 0x200000U
#define MT8183_P1_CAM_B 3U
#define MT8183_P1_FRAME_BUFFERS 7U

/*
 * Wire-only extraction of mtk_cam.h:31-39,67-70 from the same fixed source.
 * Keep the original node order; do not import media/VB2/driver objects.
 */
enum mt8183_p1_buffer_index {
	MT8183_P1_META_IN_0 = 0,
	MT8183_P1_MAIN_STREAM_OUT,
	MT8183_P1_PACKED_BIN_OUT,
	MT8183_P1_META_OUT_0,
	MT8183_P1_META_OUT_1,
	MT8183_P1_META_OUT_2,
	MT8183_P1_META_OUT_3,
};

struct mt8183_p1_frame_wire {
	u32 frame_seq_no;
	struct dma_buffer dma_bufs[MT8183_P1_FRAME_BUFFERS];
} __packed;

enum mt8183_p1_rx_kind {
	MT8183_P1_RX_EMPTY,
	MT8183_P1_RX_RAW,
	MT8183_P1_RX_SHORT_ACK,
	MT8183_P1_RX_ACK_UNINTERPRETED,
	MT8183_P1_RX_FRAME_ACK,
};

/* A bounded observation, not a semantic completion/status or transaction. */
struct mt8183_p1_rx {
	u32 ipi_id;
	size_t len;
	enum mt8183_p1_rx_kind kind;
	bool has_cmd;
	bool has_ack;
	u8 cmd_id;
	u8 ack_cmd_id;
	u32 frame_seq_no;
	u8 bytes[MT8183_P1_RX_MAX_BYTES];
};

/*
 * Both encoders write exactly sizeof(*cmd), including zero union padding.
 * They have no side effects on error. Address ranges include all 2 MiB;
 * physical allocation, mapping, alignment and ownership are caller contracts.
 * Parameter order is SCP DMA address, then CAM IOVA (wire order is reversed).
 */
int mt8183_p1_encode_init(struct mtk_isp_scp_p1_cmd *cmd,
			u64 scp_dma, u64 cam_iova);
int mt8183_p1_encode_deinit(struct mtk_isp_scp_p1_cmd *cmd);

/*
 * Accept only IPI10/11 and len <= 288; never cast payload to a packed struct.
 * Return 0 means an observation was captured, including empty/short/unknown
 * packets. It NEVER means INIT/DEINIT or a hardware operation completed.
 * IPI11 remains raw because its receive-message semantics are not established.
 * Invalid arguments/oversize leave a zeroed result when out is non-NULL.
 * A NULL packet is allowed only with len == 0. Payload/result may overlap.
 */
int mt8183_p1_observe_rx(u32 ipi_id, const void *packet, size_t len,
			struct mt8183_p1_rx *out);

/*
 * Match only IPI10 { ACK=4, FRAME_ACK=5, sequence:u32le } with >= 6 bytes.
 * Exact u32 equality: no implicit wrap ordering, pending-request lookup,
 * replay rejection, transport completion or buffer ownership transition.
 */
int mt8183_p1_match_frame_ack(const struct mt8183_p1_rx *rx, u32 expected_sequence);

#endif
