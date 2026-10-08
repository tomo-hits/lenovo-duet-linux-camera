/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
// SPDX-License-Identifier: GPL-2.0-only
/* Pure bounded codec for the fixed ChromeOS MT8183 P1 wire layout. */
#ifdef MT8183_P1_ABI_HOST_TEST
#include <errno.h>
#include <string.h>
#else
#include <linux/errno.h>
#include <linux/string.h>
#endif
#include "mt8183_p1_abi.h"

#define ABI_ASSERT(expr) _Static_assert(expr, #expr)
#define MEMBER_OFFSET(type, member) offsetof(struct type, member)

ABI_ASSERT(sizeof(u8) == 1);
ABI_ASSERT(sizeof(u16) == 2);
ABI_ASSERT(sizeof(u32) == 4);
ABI_ASSERT(sizeof(struct dma_buffer) == 8);
ABI_ASSERT(MEMBER_OFFSET(dma_buffer, iova) == 0);
ABI_ASSERT(MEMBER_OFFSET(dma_buffer, scp_addr) == 4);
ABI_ASSERT(sizeof(struct isp_init_info) == 9);
ABI_ASSERT(sizeof(struct isp_ack_info) == 5);
ABI_ASSERT(sizeof(struct p1_img_output) == 45);
ABI_ASSERT(sizeof(struct cfg_in_param) == 27);
ABI_ASSERT(sizeof(struct cfg_main_out_param) == 48);
ABI_ASSERT(sizeof(struct cfg_resize_out_param) == 46);
ABI_ASSERT(sizeof(struct p1_config_param) == 125);
ABI_ASSERT(sizeof(struct mtk_isp_scp_p1_cmd) == MT8183_P1_CMD_BYTES);
ABI_ASSERT(MEMBER_OFFSET(mtk_isp_scp_p1_cmd, cmd_id) == 0);
ABI_ASSERT(MEMBER_OFFSET(mtk_isp_scp_p1_cmd, init_param) == 1);
ABI_ASSERT(MEMBER_OFFSET(mtk_isp_scp_p1_cmd, config_param) == 1);
ABI_ASSERT(MEMBER_OFFSET(mtk_isp_scp_p1_cmd, enabled_dmas) == 1);
ABI_ASSERT(MEMBER_OFFSET(mtk_isp_scp_p1_cmd, meta_frame) == 1);
ABI_ASSERT(MEMBER_OFFSET(mtk_isp_scp_p1_cmd, is_stream_on) == 1);
ABI_ASSERT(MEMBER_OFFSET(mtk_isp_scp_p1_cmd, ack_info) == 1);
ABI_ASSERT(MEMBER_OFFSET(mtk_isp_scp_p1_cmd, init_param.hw_module) == 1);
ABI_ASSERT(MEMBER_OFFSET(mtk_isp_scp_p1_cmd, init_param.cq_addr.iova) == 2);
ABI_ASSERT(MEMBER_OFFSET(mtk_isp_scp_p1_cmd, init_param.cq_addr.scp_addr) == 6);
ABI_ASSERT(MEMBER_OFFSET(mtk_isp_scp_p1_cmd, ack_info.cmd_id) == 1);
ABI_ASSERT(MEMBER_OFFSET(mtk_isp_scp_p1_cmd, ack_info.frame_seq_no) == 2);
ABI_ASSERT(MEMBER_OFFSET(mtk_isp_scp_p1_cmd, ack_info) +
	   sizeof(struct isp_ack_info) == MT8183_P1_ACK_MIN_BYTES);
ABI_ASSERT(sizeof(struct mt8183_p1_frame_wire) == MT8183_P1_FRAME_BYTES);
ABI_ASSERT(MEMBER_OFFSET(mt8183_p1_frame_wire, frame_seq_no) == 0);
ABI_ASSERT(MEMBER_OFFSET(mt8183_p1_frame_wire, dma_bufs) == 4);
ABI_ASSERT(MEMBER_OFFSET(mt8183_p1_frame_wire, dma_bufs[MT8183_P1_META_IN_0]) == 4);
ABI_ASSERT(MEMBER_OFFSET(mt8183_p1_frame_wire, dma_bufs[MT8183_P1_MAIN_STREAM_OUT]) == 12);
ABI_ASSERT(MEMBER_OFFSET(mt8183_p1_frame_wire, dma_bufs[MT8183_P1_PACKED_BIN_OUT]) == 20);
ABI_ASSERT(MEMBER_OFFSET(mt8183_p1_frame_wire, dma_bufs[MT8183_P1_META_OUT_0]) == 28);
ABI_ASSERT(MEMBER_OFFSET(mt8183_p1_frame_wire, dma_bufs[MT8183_P1_META_OUT_1]) == 36);
ABI_ASSERT(MEMBER_OFFSET(mt8183_p1_frame_wire, dma_bufs[MT8183_P1_META_OUT_2]) == 44);
ABI_ASSERT(MEMBER_OFFSET(mt8183_p1_frame_wire, dma_bufs[MT8183_P1_META_OUT_3]) == 52);
ABI_ASSERT(MT8183_P1_META_OUT_3 + 1 == MT8183_P1_FRAME_BUFFERS);
ABI_ASSERT(MT8183_P1_CMD_BYTES <= MT8183_P1_WRAPPER_MAX_BYTES);
ABI_ASSERT(MT8183_P1_FRAME_BYTES <= MT8183_P1_WRAPPER_MAX_BYTES);
ABI_ASSERT(MT8183_P1_WRAPPER_MAX_BYTES <= MT8183_P1_RX_MAX_BYTES);
ABI_ASSERT(ISP_CMD_INIT == 0 && ISP_CMD_CONFIG == 1 && ISP_CMD_STREAM == 2 &&
	   ISP_CMD_DEINIT == 3 && ISP_CMD_ACK == 4 && ISP_CMD_FRAME_ACK == 5 &&
	   ISP_CMD_CONFIG_META == 6 && ISP_CMD_ENQUEUE_META == 7 &&
	   ISP_CMD_RESERVED == 8);

static u32 read_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}

static void write_le32(u8 *p, u32 value)
{
	p[0] = value;
	p[1] = value >> 8;
	p[2] = value >> 16;
	p[3] = value >> 24;
}

static bool composer_range_fits(u64 address)
{
	/* Check before narrowing, without addition that could overflow u64. */
	return address <= 0xffffffffULL - (MT8183_P1_COMPOSER_BYTES - 1);
}

int mt8183_p1_encode_init(struct mtk_isp_scp_p1_cmd *cmd,
			u64 scp_dma, u64 cam_iova)
{
	u8 *bytes = (u8 *)cmd;

	if (!cmd)
		return -EINVAL;
	if (!composer_range_fits(scp_dma) || !composer_range_fits(cam_iova))
		return -ERANGE;
	memset(cmd, 0, sizeof(*cmd));
	bytes[0] = ISP_CMD_INIT;
	bytes[1] = MT8183_P1_CAM_B;
	write_le32(bytes + 2, (u32)cam_iova);
	write_le32(bytes + 6, (u32)scp_dma);
	return 0;
}

int mt8183_p1_encode_deinit(struct mtk_isp_scp_p1_cmd *cmd)
{
	if (!cmd)
		return -EINVAL;
	memset(cmd, 0, sizeof(*cmd));
	((u8 *)cmd)[0] = ISP_CMD_DEINIT;
	return 0;
}

int mt8183_p1_observe_rx(u32 ipi_id, const void *packet, size_t len,
			struct mt8183_p1_rx *out)
{
	/* Local snapshot also handles payload/result overlap without data loss. */
	struct mt8183_p1_rx observation = { 0 };
	int ret = 0;

	if (!out)
		return -EINVAL;
	if (ipi_id != MT8183_P1_IPI_CMD && ipi_id != MT8183_P1_IPI_FRAME) {
		ret = -EINVAL;
		goto done;
	}
	if (len > MT8183_P1_RX_MAX_BYTES) {
		ret = -EMSGSIZE;
		goto done;
	}
	if (!packet && len) {
		ret = -EINVAL;
		goto done;
	}
	observation.ipi_id = ipi_id;
	observation.len = len;
	if (!len)
		goto done;
	memcpy(observation.bytes, packet, len);
	observation.kind = MT8183_P1_RX_RAW;
	if (ipi_id != MT8183_P1_IPI_CMD)
		goto done;
	observation.has_cmd = true;
	observation.cmd_id = observation.bytes[0];
	if (observation.cmd_id != ISP_CMD_ACK)
		goto done;
	if (len < MT8183_P1_ACK_MIN_BYTES) {
		observation.kind = MT8183_P1_RX_SHORT_ACK;
		goto done;
	}
	observation.has_ack = true;
	observation.ack_cmd_id = observation.bytes[1];
	observation.frame_seq_no = read_le32(observation.bytes + 2);
	observation.kind = observation.ack_cmd_id == ISP_CMD_FRAME_ACK ?
		MT8183_P1_RX_FRAME_ACK : MT8183_P1_RX_ACK_UNINTERPRETED;
done:
	*out = observation;
	return ret;
}

int mt8183_p1_match_frame_ack(const struct mt8183_p1_rx *rx, u32 expected_sequence)
{
	if (!rx)
		return -EINVAL;
	if (rx->len < MT8183_P1_ACK_MIN_BYTES || rx->len > MT8183_P1_RX_MAX_BYTES)
		return -EMSGSIZE;
	/* Recheck raw bytes; a summary flag/sequence alone is not evidence. */
	if (rx->ipi_id != MT8183_P1_IPI_CMD || rx->bytes[0] != ISP_CMD_ACK ||
	    rx->bytes[1] != ISP_CMD_FRAME_ACK)
		return -EPROTO;
	if (read_le32(rx->bytes + 2) != expected_sequence)
		return -ESTALE;
	return 0;
}
