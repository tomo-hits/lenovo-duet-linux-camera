/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/of_graph.h>
#include <linux/of_irq.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/property.h>
#ifdef CONFIG_COMPAT
#include <linux/compat.h>
#endif
#include <linux/videodev2.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-async.h>
#include "mtk_seninf_reg.h"
#include "mtk_seninf_def.h"

/* Capture experiment: four-lane CSI0, mux0, 1632x1224 RAW10 only. */
static bool probe_only = true;
module_param(probe_only, bool, 0444);
MODULE_PARM_DESC(probe_only, "Refuse stream-on and register-writing controls (default true)");

static inline int is_4d1c(unsigned int port)
{
	return (port < CFG_CSI_PORT_0A);
}

static inline int is_cdphy_combo(unsigned int port)
{
	return (port == CFG_CSI_PORT_0A ||
		port == CFG_CSI_PORT_0B ||
		port == CFG_CSI_PORT_0);
}

struct sensor_cfg {
	unsigned char clock_lane;
	unsigned short num_data_lanes;
};

struct mtk_seninf {
	struct v4l2_subdev subdev;
	struct v4l2_async_notifier notifier;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_subdev_format fmt[NUM_PADS];
	struct device *dev;
	struct media_pad pads[NUM_PADS];
	struct sensor_cfg sensor[NUM_SENSORS];
	unsigned int num_clks;
	struct clk_bulk_data *clks;
	void __iomem *base_reg;
	void __iomem *rx_reg;
	unsigned char __iomem *csi2_rx[CFG_CSI_PORT_MAX_NUM];
	unsigned int port;
	unsigned int mux_sel;
	struct mutex lock;
	bool streaming;
	/* A failed synchronous put consumes the stream reference nevertheless. */
	int stop_error;
};

static unsigned int mtk_seninf_get_dpcm(struct mtk_seninf *priv)
{
	int dpcm;

	switch (priv->fmt[priv->port].format.code) {
	case MEDIA_BUS_FMT_SGRBG10_DPCM8_1X8:
	case MEDIA_BUS_FMT_SRGGB10_DPCM8_1X8:
	case MEDIA_BUS_FMT_SBGGR10_DPCM8_1X8:
	case MEDIA_BUS_FMT_SGBRG10_DPCM8_1X8:
		dpcm = 0x2a;
		break;
	default:
		dpcm = 0;
		break;
	}

	return dpcm;
}

static unsigned int mtk_seninf_map_fmt(struct mtk_seninf *priv)
{
	int fmtidx = 0;

	switch (priv->fmt[priv->port].format.code) {
	case MEDIA_BUS_FMT_SBGGR8_1X8:
	case MEDIA_BUS_FMT_SGBRG8_1X8:
	case MEDIA_BUS_FMT_SGRBG8_1X8:
	case MEDIA_BUS_FMT_SRGGB8_1X8:
		fmtidx = 0;
		break;
	case MEDIA_BUS_FMT_SGRBG10_1X10:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
	case MEDIA_BUS_FMT_SBGGR10_1X10:
	case MEDIA_BUS_FMT_SGBRG10_1X10:
		fmtidx = 1;
		break;
	case MEDIA_BUS_FMT_SGRBG10_DPCM8_1X8:
	case MEDIA_BUS_FMT_SRGGB10_DPCM8_1X8:
	case MEDIA_BUS_FMT_SBGGR10_DPCM8_1X8:
	case MEDIA_BUS_FMT_SGBRG10_DPCM8_1X8:
		fmtidx = 0;
		break;
	case MEDIA_BUS_FMT_SBGGR12_1X12:
	case MEDIA_BUS_FMT_SGBRG12_1X12:
	case MEDIA_BUS_FMT_SGRBG12_1X12:
	case MEDIA_BUS_FMT_SRGGB12_1X12:
		fmtidx = 2;
		break;
	case MEDIA_BUS_FMT_UYVY8_1X16:
	case MEDIA_BUS_FMT_VYUY8_1X16:
	case MEDIA_BUS_FMT_YUYV8_1X16:
	case MEDIA_BUS_FMT_YVYU8_1X16:
		fmtidx = 3;
		break;
	case MEDIA_BUS_FMT_JPEG_1X8:
	case MEDIA_BUS_FMT_S5C_UYVY_JPEG_1X8:
		fmtidx = 7;
		break;
	}

	return fmtidx;
}

static u32 mtk_seninf_csi_port_to_seninf(u32 port)
{
	static const u32 port_to_seninf[] = {
		[CFG_CSI_PORT_0] = SENINF_1,
		[CFG_CSI_PORT_1] = SENINF_3,
		[CFG_CSI_PORT_2] = SENINF_5,
		[CFG_CSI_PORT_0A] = SENINF_1,
		[CFG_CSI_PORT_0B] = SENINF_2,
	};
	if (WARN_ON(port >= ARRAY_SIZE(port_to_seninf)))
		return -EINVAL;

	return port_to_seninf[port];
}

static void mtk_seninf_set_mux(struct mtk_seninf *priv,
			       int seninf)
{
	unsigned int mux = priv->mux_sel;
	void __iomem *pseninf_top = priv->base_reg;
	void __iomem *pseninf = priv->base_reg + 0x1000 * mux;
	unsigned int val;
	unsigned int pix_sel_ext;
	unsigned int pix_sel;
	unsigned int hs_pol = 0;
	unsigned int vs_pol = 0;
	unsigned int pixel_mode = ONE_PIXEL_MODE;
	unsigned int input_data_type;

	/* The capture receiver owns no mux interrupt either. */
	writel(0, pseninf + SENINF1_MUX_INTEN);
	/* Enable mux */
	writel((0x7fffffff & readl(pseninf + SENINF1_MUX_CTRL)) |
		0x80000000, pseninf + SENINF1_MUX_CTRL);

	input_data_type = mtk_seninf_map_fmt(priv);
	/* Set mux ctrl */
	writel((0xffff0fff & readl(pseninf + SENINF1_MUX_CTRL)) |
		0x8000, pseninf + SENINF1_MUX_CTRL);

	writel((0xfffffffc & readl(pseninf + SENINF1_MUX_CTRL_EXT)) |
		0x1, pseninf + SENINF1_MUX_CTRL_EXT);

	switch (pixel_mode) {
	case 1: /* 2 Pixel */
		pix_sel_ext = 0;
		pix_sel = 1 << 8;
		break;
	case 2: /* 4 Pixel */
		pix_sel_ext = 1 << 4;
		pix_sel = 0;
		break;
	default: /* 1 Pixel */
		pix_sel_ext = 0;
		pix_sel = 0;
		break;
	}

	writel((0xffffffef & readl(pseninf + SENINF1_MUX_CTRL_EXT)) |
		pix_sel_ext, pseninf + SENINF1_MUX_CTRL_EXT);
	writel((0xfffffeff & readl(pseninf + SENINF1_MUX_CTRL)) |
		pix_sel, pseninf + SENINF1_MUX_CTRL);

	val = 0;
	if (input_data_type != JPEG_FMT)
		val = 0x20000000;

	writel((0xcfffffff & readl(pseninf + SENINF1_MUX_CTRL)) |
		val, pseninf + SENINF1_MUX_CTRL);

	if (input_data_type != JPEG_FMT)
		writel((0xf000ffff & readl(pseninf + SENINF1_MUX_CTRL)) |
			0x6df0000, pseninf + SENINF1_MUX_CTRL);
	else
		writel((0xf000ffff & readl(pseninf + SENINF1_MUX_CTRL)) |
			0x61e0000, pseninf + SENINF1_MUX_CTRL);

	writel((0xfffff9ff & readl(pseninf + SENINF1_MUX_CTRL)) |
		(hs_pol << 10) | (vs_pol << 9), pseninf + SENINF1_MUX_CTRL);

	val = (readl(pseninf + SENINF1_MUX_CTRL) | 0x3) & 0xFFFFFFFC;
	writel(val, pseninf + SENINF1_MUX_CTRL);

	/* Set top mux */
	val = (readl(pseninf_top + SENINF_TOP_MUX_CTRL) &
		(~(0xF << (mux * 4))))	| ((seninf & 0xF) << (mux * 4));
	writel(val, pseninf + SENINF_TOP_MUX_CTRL);
}

static void mtk_seninf_set_dphy(struct mtk_seninf *priv, unsigned int seninf)
{
	void __iomem *pmipi_rx_base = priv->csi2_rx[CFG_CSI_PORT_0];
	unsigned int port = priv->port;
	void __iomem *pmipi_rx = priv->csi2_rx[port];
	void __iomem *pmipi_rx_conf = priv->base_reg + 0x1000 * seninf;

	/* Set analog phy mode to DPHY */
	if (is_cdphy_combo(port))
		writel(0xfffffffe & readl(pmipi_rx + MIPI_RX_ANA00_CSI0A),
		       pmipi_rx + MIPI_RX_ANA00_CSI0A);

	/* 4D1C: MIPIRX_ANALOG_A_BASE = 0x00001A40 */
	if (is_4d1c(port))
		writel((0xffffe49f & readl(pmipi_rx + MIPI_RX_ANA00_CSI0A)) |
			0x1a40, pmipi_rx + MIPI_RX_ANA00_CSI0A);
	else /* MIPIRX_ANALOG_BASE = 0x100 */
		writel((0xffffe49f & readl(pmipi_rx + MIPI_RX_ANA00_CSI0A)) |
			0x100, pmipi_rx + MIPI_RX_ANA00_CSI0A);

	if (is_cdphy_combo(port))
		writel(0xfffffffe & readl(pmipi_rx + MIPI_RX_ANA00_CSI0B),
			pmipi_rx + MIPI_RX_ANA00_CSI0B);

	/* Only 4d1c need set CSIB: MIPIRX_ANALOG_B_BASE = 0x00001240 */
	if (is_4d1c(port))
		writel((0xffffe49f & readl(pmipi_rx + MIPI_RX_ANA00_CSI0B)) |
			0x1240, pmipi_rx + MIPI_RX_ANA00_CSI0B);
	else /* MIPIRX_ANALOG_BASE = 0x100 */
		writel((0xffffe49f & readl(pmipi_rx + MIPI_RX_ANA00_CSI0B)) |
			0x100, pmipi_rx + MIPI_RX_ANA00_CSI0B);

	/* Byte clock invert */
	writel((0xfffffff8 & readl(pmipi_rx + MIPI_RX_ANAA8_CSI0A)) |
		0x7, pmipi_rx + MIPI_RX_ANAA8_CSI0A);
	if (is_4d1c(port))
		writel((0xfffffff8 & readl(pmipi_rx + MIPI_RX_ANAA8_CSI0B)) |
			0x7, pmipi_rx + MIPI_RX_ANAA8_CSI0B);

	/* Start ANA EQ tuning */
	if (is_cdphy_combo(port)) {
		writel((0xffffff0f & readl(pmipi_rx + MIPI_RX_ANA18_CSI0A)) |
			0x50, pmipi_rx + MIPI_RX_ANA18_CSI0A);
		writel((0xff0fffff & readl(pmipi_rx + MIPI_RX_ANA1C_CSI0A)) |
			0x500000, pmipi_rx + MIPI_RX_ANA1C_CSI0A);
		writel((0xff0fffff & readl(pmipi_rx + MIPI_RX_ANA20_CSI0A)) |
			0x500000, pmipi_rx + MIPI_RX_ANA20_CSI0A);
		if (is_4d1c(port)) { /* 4d1c */
			writel((0xffffff0f &
				readl(pmipi_rx + MIPI_RX_ANA18_CSI0B)) |
				0x50, pmipi_rx + MIPI_RX_ANA18_CSI0B);
			writel((0xff0fffff &
				readl(pmipi_rx + MIPI_RX_ANA1C_CSI0B)) |
				0x500000, pmipi_rx + MIPI_RX_ANA1C_CSI0B);
			writel((0xff0fffff &
				readl(pmipi_rx + MIPI_RX_ANA20_CSI0B)) |
				0x500000, pmipi_rx + MIPI_RX_ANA20_CSI0B);
		}
	} else {
		writel((0xff0fff0f & readl(pmipi_rx + MIPI_RX_ANA18_CSI1A)) |
			0x500050, pmipi_rx + MIPI_RX_ANA18_CSI1A);
		writel((0xffffff0f & readl(pmipi_rx + MIPI_RX_ANA1C_CSI1A)) |
			0x50, pmipi_rx + MIPI_RX_ANA1C_CSI1A);

		if (is_4d1c(port)) { /* 4d1c */
			writel((0xff0fff0f &
				readl(pmipi_rx + MIPI_RX_ANA18_CSI1B)) |
				0x500050, pmipi_rx + MIPI_RX_ANA18_CSI1B);
			writel((0xffffff0f &
				readl(pmipi_rx + MIPI_RX_ANA1C_CSI1B)) |
				0x50, pmipi_rx + MIPI_RX_ANA1C_CSI1B);
		}
	}

	/* End ANA EQ tuning */
	writel(0x90, pmipi_rx_base + MIPI_RX_ANA40_CSI0A);
	writel((0xffffff & readl(pmipi_rx + MIPI_RX_ANA24_CSI0A)) |
		0x40000000, pmipi_rx + MIPI_RX_ANA24_CSI0A);
	if (is_4d1c(port))
		writel((0xffffff & readl(pmipi_rx + MIPI_RX_ANA24_CSI0B)) |
			0x40000000, pmipi_rx + MIPI_RX_ANA24_CSI0B);
	writel(0xfffcffff & readl(pmipi_rx + MIPI_RX_WRAPPER80_CSI0A),
	       pmipi_rx + MIPI_RX_WRAPPER80_CSI0A);
	if (is_4d1c(port))
		writel(0xfffcffff & readl(pmipi_rx + MIPI_RX_WRAPPER80_CSI0B),
		       pmipi_rx + MIPI_RX_WRAPPER80_CSI0B);
	/* ANA power on */
	writel((0xfffffff7 & readl(pmipi_rx + MIPI_RX_ANA00_CSI0A)) |
		0x8, pmipi_rx + MIPI_RX_ANA00_CSI0A);
	if (is_4d1c(port))
		writel((0xfffffff7 & readl(pmipi_rx + MIPI_RX_ANA00_CSI0B)) |
			0x8, pmipi_rx + MIPI_RX_ANA00_CSI0B);

	usleep_range(20, 40);
	writel((0xfffffff7 & readl(pmipi_rx + MIPI_RX_ANA00_CSI0A)) |
		0x8, pmipi_rx + MIPI_RX_ANA00_CSI0A);
	if (is_4d1c(port))
		writel((0xfffffffb & readl(pmipi_rx + MIPI_RX_ANA00_CSI0B)) |
			0x4, pmipi_rx + MIPI_RX_ANA00_CSI0B);

	udelay(1);
	/* 4d1c: MIPIRX_CONFIG_CSI_BASE = 0xC9000000; */
	if (is_4d1c(port)) {
		writel((0xffffff &
			readl(pmipi_rx_conf + MIPI_RX_CON24_CSI0)) |
			0xc9000000, pmipi_rx_conf + MIPI_RX_CON24_CSI0);
	} else { /* 2d1c: MIPIRX_CONFIG_CSI_BASE = 0xE4000000; */
		writel((0xffffff &
			readl(pmipi_rx_conf + MIPI_RX_CON24_CSI0)) |
			0xe4000000, pmipi_rx_conf + MIPI_RX_CON24_CSI0);
	}
}

static void mtk_seninf_set_csi_mipi(struct mtk_seninf *priv,
				    unsigned int seninf)
{
	void __iomem *seninf_base = priv->base_reg;
	void __iomem *pseninf = priv->base_reg + 0x1000 * seninf;
	unsigned int dpcm = mtk_seninf_get_dpcm(priv);
	unsigned int data_lane_num = priv->sensor[priv->port].num_data_lanes;
	unsigned int data_header_order = 1;
	unsigned int pad_sel = PAD_10BIT;
	unsigned int val = 0;

	/* Clear interrupt enables before enabling the receiver hardware. */
	writel(0, pseninf + SENINF1_CSI2_INT_EN);
	writel(0, pseninf + SENINF1_CSI2_INT_EN_EXT);
	dev_dbg(priv->dev, "IS_4D1C %d port %d\n",
		is_4d1c(priv->port), priv->port);

	switch (priv->port) {
	case CFG_CSI_PORT_1:
		writel((0x7ffff8fe & readl(seninf_base +
			SENINF_TOP_PHY_SENINF_CTL_CSI1)) | 0x80000200,
		       seninf_base + SENINF_TOP_PHY_SENINF_CTL_CSI1);
		break;
	case CFG_CSI_PORT_2:
		writel((0x7ffff8fe & readl(seninf_base +
			SENINF_TOP_PHY_SENINF_CTL_CSI2)) | 0x80000200,
		       seninf_base + SENINF_TOP_PHY_SENINF_CTL_CSI2);
		break;
	case CFG_CSI_PORT_0:
		writel((0x7ffff8fe & readl(seninf_base +
			SENINF_TOP_PHY_SENINF_CTL_CSI0)) | 0x80000200,
		       seninf_base + SENINF_TOP_PHY_SENINF_CTL_CSI0);
		break;
	case CFG_CSI_PORT_0A:
	case CFG_CSI_PORT_0B:
		writel((0x7fffc8fe & readl(seninf_base +
			SENINF_TOP_PHY_SENINF_CTL_CSI0)) | 0x80001100,
		       seninf_base + SENINF_TOP_PHY_SENINF_CTL_CSI0);
		break;
	}

	/* First Enable Sensor interface and select pad (0x1a04_0200) */
	writel(readl(pseninf + SENINF1_CTRL) | 0x1,
	       pseninf + SENINF1_CTRL);
	writel((0x8fffffff & readl(pseninf + SENINF1_CTRL)) |
		(pad_sel << 28), pseninf + SENINF1_CTRL);
	writel(0xffff0fff & readl(pseninf + SENINF1_CTRL),
	       pseninf + SENINF1_CTRL);
	writel((0xffffff9f & readl(pseninf + SENINF1_CTRL_EXT)) |
		0x40, pseninf + SENINF1_CTRL_EXT);

	mtk_seninf_set_dphy(priv, seninf);

	/* DPCM Enable */
	val = 1 << ((dpcm == 0x2a) ? 15 : ((dpcm & 0xF) + 7));
	writel(val, pseninf + SENINF1_CSI2_DPCM);

	/* Settle delay */
	writel((0xffff00ff & readl(pseninf + SENINF1_CSI2_LNRD_TIMING)) |
		(SENINF_SETTLE_DELAY << 8), pseninf + SENINF1_CSI2_LNRD_TIMING);
	/* CSI2 control */
	val = readl(pseninf + SENINF1_CSI2_CTL) | (data_header_order << 16) |
		0x10 | ((1 << data_lane_num) - 1);
	writel(val, pseninf + SENINF1_CSI2_CTL);
	writel((0xfffff3f8 & readl(pseninf + SENINF1_CSI2_RESYNC_MERGE_CTL)) |
		0x3, pseninf + SENINF1_CSI2_RESYNC_MERGE_CTL);
	writel(0xfffff800 & readl(pseninf + SENINF1_CSI2_MODE),
	       pseninf + SENINF1_CSI2_MODE);
	writel(0x1dff00, pseninf + SENINF1_CSI2_DPHY_SYNC);
	writel(0xfffffffe & readl(pseninf + SENINF1_CSI2_SPARE0),
	       pseninf + SENINF1_CSI2_SPARE0);
	writel((0xf5ffff7f & readl(pseninf + SENINF1_CSI2_CTL)) |
	       0x2000000, pseninf + SENINF1_CSI2_CTL);
	writel((0xffffff00 & readl(pseninf + SENINF1_CSI2_HS_TRAIL)) |
	       SENINF_HS_TRAIL_PARAMETER, pseninf + SENINF1_CSI2_HS_TRAIL);

	/* Set debug port to output packet number */
	writel(0x8000001A, pseninf + SENINF1_CSI2_DGB_SEL);
	/* No IRQ is requested by this receiver: leave both enable masks clear. */
	writel(0, pseninf + SENINF1_CSI2_INT_EN);
	/* Write clear CSI2 IRQ */
	writel(0xffffffff, pseninf + SENINF1_CSI2_INT_STATUS);
	writel(0, pseninf + SENINF1_CSI2_INT_EN_EXT);
	writel((0xffffff7f & readl(pseninf + SENINF1_CTRL)) |
		0x80, pseninf + SENINF1_CTRL);

	udelay(1);
	writel(0xffffff7f & readl(pseninf + SENINF1_CTRL)
		, pseninf + SENINF1_CTRL);
}

static int mtk_seninf_power_on(struct mtk_seninf *priv)
{
	void __iomem *pseninf = priv->base_reg;
	struct device *dev = priv->dev;
	int seninf;
	int ret;

	seninf = mtk_seninf_csi_port_to_seninf(priv->port);
	if (seninf < 0) {
		dev_err(dev, "seninf port mapping fail\n");
		return -EINVAL;
	}

	ret = pm_runtime_resume_and_get(priv->dev);
	if (ret < 0) {
		dev_err(priv->dev, "Failed to runtime resume: %d\n", ret);
		return ret;
	}

	/* Configure timestamp */
	writel(readl(pseninf + SENINF1_CTRL) | 0x1
		, pseninf + SENINF1_CTRL);
	writel((0xffffffbfU & readl(pseninf + SENINF1_CTRL_EXT)) |
		0x40, pseninf + SENINF1_CTRL_EXT);
	writel(SENINF_TIMESTAMP_STEP, pseninf + SENINF_TG1_TM_STP);

	mtk_seninf_set_csi_mipi(priv, (unsigned int)seninf);

	mtk_seninf_set_mux(priv, (unsigned int)seninf);

	writel(0x0, pseninf + SENINF_TOP_CAM_MUX_CTRL);

	return 0;
}

static int mtk_seninf_power_off(struct mtk_seninf *priv)
{
	void __iomem *pmipi_rx = priv->csi2_rx[priv->port];
	unsigned int seninf = mtk_seninf_csi_port_to_seninf(priv->port);
	void __iomem *pseninf = priv->base_reg + 0x1000 * seninf;
	unsigned long flags;
	int ret;

	writel(0, priv->base_reg + 0x1000 * priv->mux_sel + SENINF1_MUX_INTEN);
	writel(0, pseninf + SENINF1_CSI2_INT_EN);
	writel(0, pseninf + SENINF1_CSI2_INT_EN_EXT);
	/* Disable CSI2(2.5G) first */
	writel(readl(pseninf + SENINF1_CSI2_CTL) & 0xFFFFFFE0
		, pseninf + SENINF1_CSI2_CTL);
	/* Disable mipi BG */
	switch (priv->port) {
	case CFG_CSI_PORT_0A:
		writel(0xfffffff3 & readl(pmipi_rx + MIPI_RX_ANA00_CSI0A)
			, pmipi_rx + MIPI_RX_ANA00_CSI0A);
		break;
	case CFG_CSI_PORT_0B:
		writel(0xfffffff3 & readl(pmipi_rx + MIPI_RX_ANA00_CSI0B)
			, pmipi_rx + MIPI_RX_ANA00_CSI0B);
		break;
	default:
		writel(0xfffffff3 & readl(pmipi_rx + MIPI_RX_ANA00_CSI0A)
			, pmipi_rx + MIPI_RX_ANA00_CSI0A);
		writel(0xfffffff3 & readl(pmipi_rx + MIPI_RX_ANA00_CSI0B)
			, pmipi_rx + MIPI_RX_ANA00_CSI0B);
		break;
	}

	/* RPM_GET_PUT drops usage even on failure; never repeat this put. */
	ret = pm_runtime_put_sync_suspend(priv->dev);
	priv->streaming = false;
	if (ret >= 0) {
		/* A successful put can leave another user's reference active. */
		spin_lock_irqsave(&priv->dev->power.lock, flags);
		ret = pm_runtime_suspended(priv->dev) &&
			!atomic_read(&priv->dev->power.usage_count) ? 0 : -EBUSY;
		spin_unlock_irqrestore(&priv->dev->power.lock, flags);
	}
	priv->stop_error = ret;
	return ret;
}

static const struct v4l2_mbus_framefmt mtk_seninf_default_fmt = {
	.code = MEDIA_BUS_FMT_SRGGB10_1X10,
	.width = DEFAULT_WIDTH,
	.height = DEFAULT_HEIGHT,
	.field = V4L2_FIELD_NONE,
	.colorspace = V4L2_COLORSPACE_RAW,
	.xfer_func = V4L2_XFER_FUNC_NONE,
	.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT,
	.quantization = V4L2_QUANTIZATION_FULL_RANGE,
};

static void init_fmt(struct mtk_seninf *priv)
{
	unsigned int i;

	for (i = 0; i < NUM_PADS; i++)
		priv->fmt[i].format = mtk_seninf_default_fmt;
}

static int seninf_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *mf;
	unsigned int i;

	for (i = 0; i < sd->entity.num_pads; i++) {
		mf = v4l2_subdev_state_get_format(state, i);
		*mf = mtk_seninf_default_fmt;
	}

	return 0;
}

/* The capture frontend preserves CFA order; no colour conversion here. */
static const u32 dcv_bayer_codes[] = { MEDIA_BUS_FMT_SBGGR10_1X10,
 MEDIA_BUS_FMT_SGBRG10_1X10, MEDIA_BUS_FMT_SGRBG10_1X10, MEDIA_BUS_FMT_SRGGB10_1X10 };
static int dcv_bayer_index(u32 code)
{ unsigned int i; for(i=0;i<4;i++)if(dcv_bayer_codes[i]==code)return i;return -EINVAL; }
static int seninf_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct mtk_seninf *priv = container_of(sd, struct mtk_seninf, subdev);

	if (fmt->pad >= NUM_PADS ||
	    (fmt->which != V4L2_SUBDEV_FORMAT_TRY &&
	     fmt->which != V4L2_SUBDEV_FORMAT_ACTIVE))
		return -EINVAL;

	if (dcv_bayer_index(fmt->format.code)<0 ||
	    !((fmt->format.width==1600&&fmt->format.height==1200)||(fmt->format.width==1632&&fmt->format.height==1224)||(fmt->format.width==3264&&fmt->format.height==2448)) ||
	    (fmt->format.field != V4L2_FIELD_ANY &&
	     fmt->format.field != V4L2_FIELD_NONE))
		return -EINVAL;
	{u32 code=fmt->format.code,w=fmt->format.width,h=fmt->format.height;fmt->format = mtk_seninf_default_fmt;fmt->format.code=code;fmt->format.width=w;fmt->format.height=h;}

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		*v4l2_subdev_state_get_format(state, fmt->pad) = fmt->format;
  /* Fixed rear CSI0 -> mux0 receiver does not convert the Bayer order. */
  if (fmt->pad < 2)
   *v4l2_subdev_state_get_format(state, CAM_MUX_IDX_MIN) = fmt->format;
	} else {
		mutex_lock(&priv->lock);
		if (priv->streaming) {
			mutex_unlock(&priv->lock);
			return -EBUSY;
		}
		priv->fmt[fmt->pad].pad = fmt->pad;
		priv->fmt[fmt->pad].format = fmt->format;
  if (fmt->pad < 2)
   priv->fmt[CAM_MUX_IDX_MIN].format = fmt->format;
		mutex_unlock(&priv->lock);
	}

	return 0;
}

static int seninf_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct mtk_seninf *priv = container_of(sd, struct mtk_seninf, subdev);

	if (fmt->pad >= NUM_PADS ||
	    (fmt->which != V4L2_SUBDEV_FORMAT_TRY &&
	     fmt->which != V4L2_SUBDEV_FORMAT_ACTIVE))
		return -EINVAL;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		fmt->format = *v4l2_subdev_state_get_format(state, fmt->pad);
	} else {
		mutex_lock(&priv->lock);
		fmt->format.code = priv->fmt[fmt->pad].format.code;
		fmt->format.width = priv->fmt[fmt->pad].format.width;
		fmt->format.height = priv->fmt[fmt->pad].format.height;
		fmt->format.field = priv->fmt[fmt->pad].format.field;
		fmt->format.colorspace = priv->fmt[fmt->pad].format.colorspace;
		fmt->format.xfer_func = priv->fmt[fmt->pad].format.xfer_func;
		fmt->format.ycbcr_enc = priv->fmt[fmt->pad].format.ycbcr_enc;
		fmt->format.quantization =
			priv->fmt[fmt->pad].format.quantization;
		mutex_unlock(&priv->lock);
	}

	return 0;
}

static int seninf_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct mtk_seninf *priv = container_of(sd, struct mtk_seninf, subdev);

	if (code->pad >= NUM_PADS || code->index>=4)
		return -EINVAL;
	mutex_lock(&priv->lock);
	code->code = dcv_bayer_codes[code->index];
	mutex_unlock(&priv->lock);

	return 0;
}

static int seninf_s_stream(struct v4l2_subdev *sd, int on)
{
	struct mtk_seninf *priv = container_of(sd, struct mtk_seninf, subdev);
	int ret = 0;

	mutex_lock(&priv->lock);
	if (priv->stop_error) {
		ret = priv->stop_error;
		goto out;
	}
	if (!!on == priv->streaming)
		goto out;
	if (on && probe_only) {
		ret = -EACCES;
		goto out;
	}
	if (on && (priv->port > CFG_CSI_PORT_1 || priv->mux_sel ||
		   priv->sensor[priv->port].num_data_lanes != (priv->port == CFG_CSI_PORT_0 ? 4 : 1))) {
		ret = -ENOLINK;
		goto out;
	}
	if (on)
		ret = mtk_seninf_power_on(priv);
	else
		ret = mtk_seninf_power_off(priv);
	if (!ret)
		priv->streaming = !!on;
out:
	mutex_unlock(&priv->lock);

	return ret;
};

static const struct v4l2_subdev_pad_ops seninf_subdev_pad_ops = {
	.set_fmt = seninf_set_fmt,
	.get_fmt = seninf_get_fmt,
	.enum_mbus_code = seninf_enum_mbus_code,
};

static const struct v4l2_subdev_internal_ops seninf_internal_ops = {
	.init_state = seninf_init_state,
};

static const struct v4l2_subdev_video_ops seninf_subdev_video_ops = {
	.s_stream = seninf_s_stream,
};

static struct v4l2_subdev_core_ops seninf_subdev_core_ops = {
	.subscribe_event    = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event	= v4l2_event_subdev_unsubscribe,
};

static struct v4l2_subdev_ops seninf_subdev_ops = {
	.core	= &seninf_subdev_core_ops,
	.video	= &seninf_subdev_video_ops,
	.pad	= &seninf_subdev_pad_ops,
};

static int seninf_link_setup(struct media_entity *entity,
			     const struct media_pad *local,
			     const struct media_pad *remote, u32 flags)
{
	struct v4l2_subdev *sd;
	struct mtk_seninf *priv;
	struct device *dev;

	sd = media_entity_to_v4l2_subdev(entity);
	priv = v4l2_get_subdevdata(sd);
	dev = priv->dev;
	dev_dbg(dev, "mtk_seninf: remote %d-%d, local %d-%d\n"
		, remote->entity->graph_obj.id, remote->index
		, local->entity->graph_obj.id, local->index);
	dev_dbg(dev, "local->flags %lu flags %u\n", local->flags, flags);

	if (local->index >= NUM_PADS)
		return -EINVAL;
	mutex_lock(&priv->lock);
	if (priv->streaming) {
		mutex_unlock(&priv->lock);
		return -EBUSY;
	}
	if ((local->flags & MEDIA_PAD_FL_SOURCE) &&
	    (flags & MEDIA_LNK_FL_ENABLED)) {
		dev_dbg(dev, "set cam mux %d\n", local->index);
		priv->mux_sel = local->index - CAM_MUX_IDX_MIN;
	}

	if ((local->flags & MEDIA_PAD_FL_SINK) &&
	    (flags & MEDIA_LNK_FL_ENABLED)) {
		dev_dbg(dev, "set sensor port %d\n", local->index);
		/* One physical capture engine: links select a single input. */
        if(priv->port < NUM_SENSORS && priv->port != local->index){mutex_unlock(&priv->lock);return -EBUSY;}
		priv->port = local->index;
		if (priv->port >= NUM_SENSORS) {
			dev_err(dev, "port index is over number of ports\n");
			mutex_unlock(&priv->lock);
			return -EINVAL;
		}
	}
	if ((local->flags & MEDIA_PAD_FL_SINK) &&
	    !(flags & MEDIA_LNK_FL_ENABLED) && priv->port == local->index)
		priv->port = NUM_SENSORS;
	mutex_unlock(&priv->lock);

	return 0;
}

static const struct media_entity_operations seninf_media_ops = {
	.link_setup = seninf_link_setup,
	.link_validate = v4l2_subdev_link_validate,
};

struct sensor_async_connection {
	struct v4l2_async_connection asc;
	u32 port;
	u32 lanes;
};

static int mtk_seninf_notifier_bound
			(struct v4l2_async_notifier *notifier,
			 struct v4l2_subdev *sd,
			 struct v4l2_async_connection *asc)
{
	struct mtk_seninf *priv =
		container_of(notifier, struct mtk_seninf, notifier);
	struct sensor_async_connection *sensor =
		container_of(asc, struct sensor_async_connection, asc);
	int source_pad, ret;

	dev_dbg(priv->dev, "%s bounded with port:%d lanes: %d\n",
		sd->entity.name, sensor->port, sensor->lanes);

	source_pad = media_entity_get_fwnode_pad(&sd->entity, asc->match.fwnode,
					       MEDIA_PAD_FL_SOURCE);
	if (source_pad < 0)
		return source_pad;

	ret = media_create_pad_link(&sd->entity, source_pad,
				    &priv->subdev.entity, sensor->port, 0);
	if (ret) {
		dev_err(priv->dev, "failed to create link for %s\n",
			sd->entity.name);
		return ret;
	}
	mutex_lock(&priv->lock);
	priv->sensor[sensor->port].num_data_lanes = sensor->lanes;
	mutex_unlock(&priv->lock);

	return 0;
}

static void mtk_seninf_notifier_unbind(struct v4l2_async_notifier *notifier,
				     struct v4l2_subdev *sd,
				     struct v4l2_async_connection *asc)
{
	struct mtk_seninf *priv =
		container_of(notifier, struct mtk_seninf, notifier);
	struct sensor_async_connection *sensor =
		container_of(asc, struct sensor_async_connection, asc);

	mutex_lock(&priv->lock);
	if (priv->port == sensor->port) {
		if (priv->streaming)
			mtk_seninf_power_off(priv);
		priv->streaming = false;
		priv->port = NUM_SENSORS;
	}
	priv->sensor[sensor->port].num_data_lanes = 0;
	mutex_unlock(&priv->lock);
}

static const struct v4l2_async_notifier_operations mtk_seninf_async_ops = {
	.bound = mtk_seninf_notifier_bound,
	.unbind = mtk_seninf_notifier_unbind,
};

static int mtk_seninf_parse_endpoints(struct mtk_seninf *priv)
{
	struct fwnode_handle *ep;
	unsigned long ports = 0;
	int ret;

	fwnode_graph_for_each_endpoint(dev_fwnode(priv->dev), ep) {
		struct v4l2_fwnode_endpoint vep = {
			.bus_type = V4L2_MBUS_CSI2_DPHY,
		};
		struct fwnode_endpoint base = { 0 };
		struct sensor_async_connection *sensor;
		unsigned int i, lanes;

		ret = fwnode_graph_parse_endpoint(ep, &base);
		if (ret)
			goto err_put;
		if (base.port >= NUM_PADS || base.id) {
			ret = -EINVAL;
			goto err_put;
		}
		/* Only sink ports describe CSI-2 input bus wiring. */
		if (base.port >= NUM_SENSORS)
			continue;
		ret = v4l2_fwnode_endpoint_parse(ep, &vep);
		if (ret)
			goto err_put;
		lanes = vep.bus.mipi_csi2.num_data_lanes;
		if (vep.bus_type != V4L2_MBUS_CSI2_DPHY || !lanes ||
		    lanes > (is_4d1c(base.port) ? 4 : 2) ||
		    vep.bus.mipi_csi2.clock_lane || test_bit(base.port, &ports)) {
			ret = -EINVAL;
			goto err_put;
		}
		/* The inherited register sequence does not remap lanes/polarity. */
		for (i = 0; i <= lanes; i++) {
			if (vep.bus.mipi_csi2.lane_polarities[i] ||
			    (i && vep.bus.mipi_csi2.data_lanes[i - 1] != i)) {
				ret = -EINVAL;
				goto err_put;
			}
		}
		sensor = v4l2_async_nf_add_fwnode_remote(&priv->notifier, ep,
						 struct sensor_async_connection);
		if (IS_ERR(sensor)) {
			ret = PTR_ERR(sensor);
			goto err_put;
		}
		sensor->port = base.port;
		sensor->lanes = lanes;
		set_bit(base.port, &ports);
	}

	return ports ? 0 : -ENOTCONN;

err_put:
	fwnode_handle_put(ep);
	return ret;
}

static int seninf_set_ctrl(struct v4l2_ctrl *ctrl)
{
	/* Capture owns the only register-writing path; no debug/test generator. */
	return -EOPNOTSUPP;
}

static const struct v4l2_ctrl_ops seninf_ctrl_ops = {
	.s_ctrl = seninf_set_ctrl,
};

static const char * const seninf_test_pattern_menu[] = {
	"Horizontal bars",
	"Monitor status",
};

static int seninf_initialize_controls(struct mtk_seninf *priv)
{
	struct v4l2_ctrl_handler *handler;
	int ret;

	handler = &priv->ctrl_handler;
	ret = v4l2_ctrl_handler_init(handler, 2);
	if (ret)
		return ret;
	handler->lock = &priv->lock;
	v4l2_ctrl_new_std_menu_items(handler, &seninf_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(seninf_test_pattern_menu) - 1,
				     0, 0, seninf_test_pattern_menu);

	if (handler->error) {
		ret = handler->error;
		dev_err(priv->dev,
			"Failed to init controls(%d)\n", ret);
		goto err_free_handler;
	}

	priv->subdev.ctrl_handler = handler;
	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

static int mtk_seninf_media_register(struct mtk_seninf *priv)
{
	struct v4l2_subdev *sd = &priv->subdev;
	struct media_pad *pads = priv->pads;
	struct device *dev = priv->dev;
	int i;
	int ret;

	v4l2_subdev_init(sd, &seninf_subdev_ops);
	sd->internal_ops = &seninf_internal_ops;

	init_fmt(priv);
	ret = seninf_initialize_controls(priv);
	if (ret) {
		dev_err(dev, "Failed to initialize controls\n");
		return ret;
	}

	sd->flags |= (V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS);

	priv->subdev.dev = dev;
	snprintf(sd->name, sizeof(sd->name), "%s",
		 dev_name(dev));
	v4l2_set_subdevdata(sd, priv);

	sd->entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sd->entity.ops = &seninf_media_ops;

	for (i = 0; i < NUM_SENSORS; i++)
		pads[i].flags = MEDIA_PAD_FL_SINK;

	for (i = CAM_MUX_IDX_MIN; i < NUM_PADS; i++)
		pads[i].flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sd->entity, NUM_PADS, pads);
	if (ret < 0)
		goto err_free_handler;

	v4l2_async_subdev_nf_init(&priv->notifier, sd);
	ret = mtk_seninf_parse_endpoints(priv);
	if (ret)
		goto err_clean_notifier;

	priv->notifier.ops = &mtk_seninf_async_ops;
	ret = v4l2_async_nf_register(&priv->notifier);
	if (ret < 0) {
		dev_err(dev, "v4l2 async notifier register failed\n");
		goto err_clean_notifier;
	}

	ret = v4l2_async_register_subdev(sd);
	if (ret < 0) {
		dev_err(dev, "v4l2 async register subdev failed\n");
		goto err_unregister_notifier;
	}
	return 0;

err_unregister_notifier:
	v4l2_async_nf_unregister(&priv->notifier);
err_clean_notifier:
	v4l2_async_nf_cleanup(&priv->notifier);
	media_entity_cleanup(&sd->entity);
err_free_handler:
	v4l2_ctrl_handler_free(&priv->ctrl_handler);

	return ret;
}

static int seninf_probe(struct platform_device *pdev)
{
	/* List of clocks required by seninf */
	static const char * const clk_names[] = {
		"CLK_CAM_SENINF", "CLK_TOP_MUX_SENINF"
	};
	struct resource *res;
	struct mtk_seninf *priv;
	struct device *dev = &pdev->dev;
	int i, ret;

	priv = devm_kzalloc(dev, sizeof(struct mtk_seninf), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	dev_set_drvdata(dev, priv);
	priv->dev = dev;
	priv->port = NUM_SENSORS;
	mutex_init(&priv->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) < 0x8000)
		return -EINVAL;
	priv->base_reg = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->base_reg))
		return PTR_ERR(priv->base_reg);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res || resource_size(res) < 0x6000)
		return -EINVAL;
	priv->rx_reg = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->rx_reg))
		return PTR_ERR(priv->rx_reg);

	priv->csi2_rx[CFG_CSI_PORT_0]  = priv->rx_reg;
	priv->csi2_rx[CFG_CSI_PORT_0A] = priv->rx_reg;
	priv->csi2_rx[CFG_CSI_PORT_0B] = priv->rx_reg + 0x1000;
	priv->csi2_rx[CFG_CSI_PORT_1]  = priv->rx_reg + 0x2000;
	priv->csi2_rx[CFG_CSI_PORT_2]  = priv->rx_reg + 0x4000;

	priv->num_clks = ARRAY_SIZE(clk_names);
	priv->clks = devm_kcalloc(dev, priv->num_clks,
				  sizeof(*priv->clks), GFP_KERNEL);
	if (!priv->clks)
		return -ENOMEM;

	for (i = 0; i < priv->num_clks; ++i)
		priv->clks[i].id = clk_names[i];

	ret = devm_clk_bulk_get(dev, priv->num_clks, priv->clks);
	if (ret) {
		dev_err(dev, "failed to get seninf clock:%d\n", ret);
		return ret;
	}

	pm_runtime_enable(dev);
	ret = mtk_seninf_media_register(priv);
	if (ret) {
		pm_runtime_disable(dev);
		return ret;
	}
	dev_info(dev, "SENINF subdev registered, probe_only=%u\n", probe_only);

	return ret;
}

static int __maybe_unused seninf_pm_suspend(struct device *dev)
{
	struct mtk_seninf *priv = dev_get_drvdata(dev);

	dev_dbg(dev, "seninf runtime suspend\n");
	clk_bulk_disable_unprepare(priv->num_clks, priv->clks);

	return 0;
}

static int __maybe_unused seninf_pm_resume(struct device *dev)
{
	struct mtk_seninf *priv = dev_get_drvdata(dev);
	int ret;

	dev_dbg(dev, "seninf runtime resume\n");
	ret = clk_bulk_prepare_enable(priv->num_clks, priv->clks);
	if (ret) {
		dev_err(dev, "failed to enable clock:%d\n", ret);
		return ret;
	}

	return 0;
}

static const struct dev_pm_ops runtime_pm_ops = {
	SET_RUNTIME_PM_OPS(seninf_pm_suspend, seninf_pm_resume, NULL)
};

static void seninf_remove(struct platform_device *pdev)
{
	struct mtk_seninf *priv = dev_get_drvdata(&pdev->dev);
	struct v4l2_subdev *subdev = &priv->subdev;

	v4l2_async_nf_unregister(&priv->notifier);
	v4l2_async_nf_cleanup(&priv->notifier);
	v4l2_async_unregister_subdev(subdev);
	seninf_s_stream(subdev, 0);
	media_entity_cleanup(&subdev->entity);
	v4l2_ctrl_handler_free(&priv->ctrl_handler);

	pm_runtime_disable(priv->dev);
	if (!pm_runtime_status_suspended(priv->dev))
		seninf_pm_suspend(priv->dev);
	pm_runtime_set_suspended(priv->dev);
}

#ifdef CONFIG_OF
static const struct of_device_id mtk_seninf_of_match[] = {
	{.compatible = "mediatek,mt8183-seninf"},
	{},
};
MODULE_DEVICE_TABLE(of, mtk_seninf_of_match);
#endif

static struct platform_driver seninf_pdrv = {
	.driver	= {
		.name	= "seninf",
		.pm  = &runtime_pm_ops,
		.of_match_table = of_match_ptr(mtk_seninf_of_match),
	},
	.probe	= seninf_probe,
	.remove	= seninf_remove,
};

module_platform_driver(seninf_pdrv);

MODULE_DESCRIPTION("MTK seninf driver");
MODULE_AUTHOR("Louis Kuo <louis.kuo@mediatek.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("MTK:mtk_seninf");
