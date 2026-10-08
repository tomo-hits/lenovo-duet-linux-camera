/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* Modified on 2026-10-06: explicit, root-loader-checked internal eMMC mode. */
// SPDX-License-Identifier: GPL-2.0-only
/* MT8183 P1 RAW video backend with explicit DMA stop proof.
 * Pinned Linux 6.18.28-mt81, SKU176, controlled single-owner test environment.
 * Derived from this project's separately reviewed no-IPI tests. Media graph
 * pad identities and packed ABI originate from MediaTek (2019), GPL-2.0,
 * ChromiumOS 527db0b5974bb70364fc692448a55fb37209fe23 isp_50/cam.
 * Probe remains no-IPI. Once notified, release requires final SCP stop and CAM OFF.
 */
#include "mt8183_p1_abi.h"
#include "mt8183_p1_legacy.h"
#include "mt8183_p1_stop.h"
#include "camera_video.h"
#include "mt8183_p1_hw.h"
#include "mt8183_p1_workers.h"
#include "mt8183_p1_handoff.h"
#include <linux/debugfs.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <linux/remoteproc.h>
#include <linux/remoteproc/mtk_scp.h>
#include <linux/spinlock.h>
#include <dt-bindings/memory/mt8183-larb-port.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/dma-direct.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu-dma.h>
#include <linux/iommu.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/suspend.h>
#include <linux/property.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/utsname.h>
#include <linux/vmalloc.h>
#include <media/media-device.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-event.h>

#if !defined(CONFIG_ARM64) || !defined(CONFIG_DMA_DECLARE_COHERENT) || \
    !defined(CONFIG_IOMMU_DMA) || !defined(CONFIG_OF_RESERVED_MEM)
#error This driver requires the fixed ARM64 coherent-pool/IOMMU configuration
#endif
#ifdef CONFIG_ARCH_HAS_DMA_MAP_DIRECT
#error Architecture DMA bypass is not part of the audited target
#endif

#if !defined(CONFIG_MEDIA_CONTROLLER)
#error Media controller support is required
#endif
#if MT8183_P1_BURST_FRAMES != 30 || MT8183_P1_IMAGE_SPANS != 6
#error P1 firmware queue geometry does not match the validated ABI
#endif
#define COMPAT "mediatek,mt8183-p1-raw"
#define NODE_PATH "/camera@1a006000"
#define NODE_NAME "camera"
#define RAM_MARKER "mt8183_camera_ram=baseline"
#define CAM_BASE 0x1a006000
#define CAM_SIZE 0x2000
#define SYSIRQ_PATH "/soc/interrupt-controller@c530a80"
#define GIC_PATH "/soc/interrupt-controller@c000000"
#define CLOCK_PATH "/soc/syscon@1a000000"
#define POWER_PATH "/soc/syscon@10006000/power-controller"

/* The root-only loader verifies the selected storage mode and exact hardware
 * before explicitly opting in. Internal mode requires the user's explicit OS
 * installation choice and direct root/boot partitions on the same MMC. These
 * read-only parameters do not change boot, firmware, or block protection.
 * The existing RAM markers remain supported. */
static bool external_usb;
module_param(external_usb, bool, 0444);
MODULE_PARM_DESC(external_usb, "Allow a protected external USB test boot after loader checks (default false)");
static bool internal_emmc;
module_param(internal_emmc, bool, 0400);
MODULE_PARM_DESC(internal_emmc, "Allow explicitly selected internal eMMC OS after root-only loader checks (default false)");

#define POOL_BASE ((phys_addr_t)0x50000000)
#define POOL_SIZE ((phys_addr_t)0x02900000)
#define TEST_SIZE SZ_2M
#define CAM_APERTURE_END (SZ_4G - SZ_8M - 1)
#define TEST_ATTRS DMA_ATTR_SKIP_CPU_SYNC
#define TEST_DIR DMA_BIDIRECTIONAL

struct composer_state {
	struct device *cam;
	struct device *scp;
	void *cpu;
	phys_addr_t physical;
	dma_addr_t scp_dma;
	dma_addr_t cam_iova;
	unsigned int mapped_pages;
	bool mapped;
};

struct resource_state {
	struct device *dev;
	void __iomem *base;
	struct clk_bulk_data clocks[2];
	int irq;
	bool region_owned, clocks_owned, irq_owned;
	bool pm_enabled, pm_ref, clocks_on;
};
#define P1_DT_PORT 0
#define P1_SINK_PAD 0
#define P1_SOURCE_PAD 1
#define P1_NUM_PADS 2
#define SENINF_SOURCE_PAD 4
#define SENINF_SENSOR_PAD 0
#define GRAPH_LINK_FLAGS (MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE)

struct mt8183_p1_graph {
	struct device *dev;
	struct media_device media;
	struct v4l2_device v4l2;
	struct v4l2_subdev subdev;
	struct media_pad pads[P1_NUM_PADS];
	struct v4l2_async_notifier notifier;
	struct v4l2_subdev *seninf;
	struct v4l2_subdev *sensor;
	bool sensor_pinned, seninf_pinned, prepared;
	struct fwnode_handle *seninf_fwnode;
	struct fwnode_handle *sensor_fwnode[2];
	bool media_initialized;
	bool media_registered;
	bool v4l2_registered;
	bool subdev_finalized;
	struct completion nodes_released;
	bool entity_initialized;
	bool subdev_registered;
	bool notifier_initialized;
	bool notifier_registered;
	bool complete;
};

struct integrated_state;
struct ipi_channel {
	struct integrated_state *state;
	u32 id, count, len;
	u8 bytes[MT8183_P1_RX_MAX_BYTES];
	struct completion reply;
};

struct integrated_state {
	struct resource_state resources;
	struct composer_state composer;
	struct mt8183_p1_graph graph;
	struct mutex control_lock;
	struct dentry *debug_dir;
	spinlock_t rx_lock;
	struct mtk_scp *scp_api;
	struct rproc *rproc;
	struct device *smi_dev;
	bool smi_ref, stopping;
	struct ipi_channel channel[2];
	struct mt8183_p1_hw hw;
    struct dcv_video video;
    bool stream_module_ref;
	struct dpw_session workers;
	struct dph_handoff handoff;
	bool workers_active;
	u64 session_epoch, retired_epoch;
	u32 rearms, retired_sessions, retired_cpu_allocations, retired_cpu_frees;
	int rearm_error;
	atomic_t irq_enabled, session_irq_calls;
	u32 cq_descriptor, cq_source, cq_shared_sequence, cq_shared_base, pre_frame_sequence;
	struct mt8183_p1_stop_gate stop_gate;
	struct mt8183_p1_stop_snapshot stop_snapshot;
	bool ipi_registered[2], scp_owned, fw_exposed, stopped;
	bool cycle_verified, stop_verified, transport_failed;
	bool video_needs_prepare;
 bool system_sleep_pending, system_notifier_registered;
 struct notifier_block system_notifier;
	unsigned int stage, commands, cycles;
	int control_error, send_error, stop_error;
	bool sensor_on, seninf_on, inputs_idle, capture_attempted, capture_ok;
	bool retain_capture;
	u32 sensor_code;
	u32 requested_exposure,requested_gain;
	int capture_error, input_error;
};

static struct dentry *p1_debug_root;
static struct device_node *test_node;
static DEFINE_MUTEX(stats_lock);
static atomic_t irq_calls = ATOMIC_INIT(0);
/* Unique within this resident module lifetime; exported with boot/module ID. */
static atomic64_t session_epochs = ATOMIC64_INIT(0);
static struct {
	unsigned int probes, removes, allocations, frees, maps, unmaps;
	unsigned int regions, regions_freed, mmaps, munmaps, clk_gets, clk_puts;
	unsigned int irq_requests, irq_frees, pm_enables, pm_disables, pm_gets, pm_puts;
	unsigned int resumes, suspends, cleanup_errors;
	unsigned int media_inits, media_cleanups, v4l2_regs, v4l2_unregs;
	unsigned int entity_inits, entity_cleanups, subdev_regs, subdev_unregs;
	unsigned int notifier_inits, notifier_cleanups, notifier_regs, notifier_unregs;
	unsigned int media_regs, media_unregs, graph_completes;
	bool active;
	int last_error;
} stats;
#define COUNT(member) do { mutex_lock(&stats_lock); stats.member++; mutex_unlock(&stats_lock); } while (0)

static void finish_stats(int error, bool active)
{
	mutex_lock(&stats_lock);
	stats.last_error = error;
	stats.active = active;
	mutex_unlock(&stats_lock);
}

static int get_stats(char *buf, const struct kernel_param *kp)
{
	int ret;

	mutex_lock(&stats_lock);
	ret = scnprintf(buf, PAGE_SIZE,
		"probes=%u removes=%u allocations=%u frees=%u maps=%u unmaps=%u regions=%u regions_freed=%u mmaps=%u munmaps=%u clk_gets=%u clk_puts=%u irq_requests=%u irq_frees=%u pm_enables=%u pm_disables=%u pm_gets=%u pm_puts=%u resumes=%u suspends=%u cleanup_errors=%u media_inits=%u media_cleanups=%u v4l2_regs=%u v4l2_unregs=%u entity_inits=%u entity_cleanups=%u subdev_regs=%u subdev_unregs=%u notifier_inits=%u notifier_cleanups=%u notifier_regs=%u notifier_unregs=%u media_regs=%u media_unregs=%u graph_completes=%u active=%u last_error=%d irq_calls=%d scope=probe_counters\n",
		stats.probes, stats.removes, stats.allocations, stats.frees,
		stats.maps, stats.unmaps, stats.regions, stats.regions_freed,
		stats.mmaps, stats.munmaps, stats.clk_gets, stats.clk_puts,
		stats.irq_requests, stats.irq_frees, stats.pm_enables, stats.pm_disables,
		stats.pm_gets, stats.pm_puts, stats.resumes, stats.suspends,
		stats.cleanup_errors, stats.media_inits, stats.media_cleanups, stats.v4l2_regs,
		stats.v4l2_unregs, stats.entity_inits, stats.entity_cleanups, stats.subdev_regs,
		stats.subdev_unregs, stats.notifier_inits, stats.notifier_cleanups, stats.notifier_regs,
		stats.notifier_unregs, stats.media_regs, stats.media_unregs, stats.graph_completes,
		stats.active, stats.last_error,
		atomic_read(&irq_calls));
	mutex_unlock(&stats_lock);
	return ret;
}


static bool path_is(struct device_node *np, const char *path)
{
	struct device_node *expected = of_find_node_by_path(path);
	bool same = np && np == expected;

	of_node_put(expected);
	return same;
}

static int validate_reference(struct device_node *np, const char *property,
			      const char *cells_name, int index,
			      const char *path, u32 id)
{
	struct of_phandle_args args;
	int ret = of_parse_phandle_with_args(np, property, cells_name, index, &args);

	if (ret)
		return ret;
	ret = path_is(args.np, path) && of_device_is_available(args.np) &&
	      args.args_count == 1 && args.args[0] == id ? 0 : -EINVAL;
	of_node_put(args.np);
	return ret;
}

static int validate_dma_node(struct device_node *target)
{
	struct device_node *np, *scp;
	struct of_phandle_args iommu;
	int ret;

	if (of_count_phandle_with_args(target, "iommus", "#iommu-cells") != 1 ||
	    of_count_phandle_with_args(target, "mediatek,scp", NULL) != 1)
		return -EINVAL;
	scp = of_parse_phandle(target, "mediatek,scp", 0);
	ret = path_is(scp, "/soc/scp@10500000") && of_device_is_available(scp) &&
	      of_device_is_compatible(scp, "mediatek,mt8183-scp") ? 0 : -EINVAL;
	of_node_put(scp);
	if (ret)
		return ret;
	ret = of_parse_phandle_with_args(target, "iommus", "#iommu-cells", 0, &iommu);
	if (ret)
		return ret;
	if (!path_is(iommu.np, "/soc/iommu@10205000") ||
	    !of_device_is_compatible(iommu.np, "mediatek,mt8183-m4u") ||
	    !of_device_is_available(iommu.np) || iommu.args_count != 1 ||
	    iommu.args[0] != M4U_PORT_CAM_IMGO) {
		ret = -EINVAL;
		goto out_iommu;
	}
	for (np = of_find_all_nodes(NULL); np; np = of_find_all_nodes(np)) {
		int i, nr;

		if (np == target)
			continue;
		nr = of_count_phandle_with_args(np, "iommus", "#iommu-cells");
		for (i = 0; i < nr; i++) {
			struct of_phandle_args other;
			bool conflict;

			ret = of_parse_phandle_with_args(np, "iommus", "#iommu-cells", i, &other);
			if (ret)
				goto out_node;
			conflict = other.np == iommu.np && other.args_count == 1 &&
				   other.args[0] == M4U_PORT_CAM_IMGO;
			of_node_put(other.np);
			if (conflict) { ret = -EBUSY; goto out_node; }
		}
	}
	ret = 0;
	goto out_iommu;
out_node:
	of_node_put(np);
out_iommu:
	of_node_put(iommu.np);
	return ret;
}

static int validate_node(struct device_node *np)
{
	struct property *p;
	struct device_node *parent, *gic;
	struct of_phandle_args irq;
	u32 reg[4], values[3];
	int ret;

	if ((!path_is(np, NODE_PATH) && !path_is(np, MT8183_P1_LEGACY_PATH)) ||
	    of_property_count_strings(np, "compatible") != 1 ||
	    (!of_device_is_compatible(np, COMPAT) &&
	     !of_device_is_compatible(np, MT8183_P1_LEGACY_COMPAT)) || !of_device_is_available(np))
		return -EINVAL;
	if ((path_is(np, NODE_PATH) && !of_device_is_compatible(np, COMPAT)) ||
	    (path_is(np, MT8183_P1_LEGACY_PATH) &&
	     !of_device_is_compatible(np, MT8183_P1_LEGACY_COMPAT)))
		return -EINVAL;
	for_each_property_of_node(np, p) {
		if (!strcmp(p->name, "name")) {
			const char *name = path_is(np, NODE_PATH) ? NODE_NAME : MT8183_P1_LEGACY_NAME;
			if (p->length != strlen(name) + 1 ||
			    memcmp(p->value, name, strlen(name) + 1))
				return -EINVAL;
			continue;
		}
		if (strcmp(p->name, "compatible") && strcmp(p->name, "status") &&
		    strcmp(p->name, "reg") && strcmp(p->name, "interrupt-parent") &&
		    strcmp(p->name, "interrupts") && strcmp(p->name, "clocks") &&
		    strcmp(p->name, "clock-names") && strcmp(p->name, "power-domains") &&
		    strcmp(p->name, "iommus") && strcmp(p->name, "mediatek,scp"))
			return -EINVAL;
	}
	if (of_property_count_u32_elems(np, "reg") != 4 ||
	    of_property_read_u32_array(np, "reg", reg, 4) ||
	    reg[0] || reg[1] != CAM_BASE || reg[2] || reg[3] != CAM_SIZE ||
	    of_property_count_u32_elems(np, "interrupts") != 3 ||
	    of_property_read_u32_array(np, "interrupts", values, 3) ||
	    values[0] || values[1] != 255 || values[2] != IRQ_TYPE_LEVEL_LOW ||
	    of_property_count_u32_elems(np, "interrupt-parent") != 1)
		return -EINVAL;
	parent = of_parse_phandle(np, "interrupt-parent", 0);
	if (!path_is(parent, SYSIRQ_PATH) || !of_device_is_available(parent) ||
	    !of_device_is_compatible(parent, "mediatek,mt8183-sysirq")) {
		of_node_put(parent);
		return -EINVAL;
	}
	gic = of_irq_find_parent(parent);
	ret = path_is(gic, GIC_PATH) && of_device_is_compatible(gic, "arm,gic-v3") ? 0 : -EINVAL;
	of_node_put(gic);
	of_node_put(parent);
	if (ret)
		return ret;
	ret = of_irq_parse_one(np, 0, &irq);
	if (ret)
		return ret;
	ret = path_is(irq.np, SYSIRQ_PATH) && irq.args_count == 3 &&
	      irq.args[0] == 0 && irq.args[1] == 255 && irq.args[2] == IRQ_TYPE_LEVEL_LOW ? 0 : -EINVAL;
	of_node_put(irq.np);
	if (ret || of_count_phandle_with_args(np, "clocks", "#clock-cells") != 2 ||
	    of_property_count_strings(np, "clock-names") != 2 ||
	    of_property_match_string(np, "clock-names", "cam") != 0 ||
	    of_property_match_string(np, "clock-names", "camtg") != 1 ||
	    of_count_phandle_with_args(np, "power-domains", "#power-domain-cells") != 1)
		return -EINVAL;
	ret = validate_reference(np, "clocks", "#clock-cells", 0, CLOCK_PATH, 2);
	if (!ret)
		ret = validate_reference(np, "clocks", "#clock-cells", 1, CLOCK_PATH, 3);
	if (!ret)
		ret = validate_reference(np, "power-domains", "#power-domain-cells", 0, POWER_PATH, 8);
	if (!ret)
		ret = validate_dma_node(np);
	return ret;
}

static int boot_mode_gate(bool ram)
{
	if (external_usb && internal_emmc)
		return -EINVAL;
	return ram || external_usb || internal_emmc ? 0 : -EPERM;
}

static int validate_tree(void)
{
	struct device_node *np, *chosen;
	const char *args, *p;
	const char * const markers[] = { RAM_MARKER, MT8183_P1_LEGACY_RAM_MARKER };
	const char *marker;
	size_t length;
	unsigned int i;
	unsigned int count = 0;
	bool ram = false;
	int ret;

	if (strcmp(init_utsname()->release, "6.18.28-mt81") ||
	    !of_machine_is_compatible("google,krane-sku176") || PAGE_SIZE != 4096)
		return -ENODEV;
	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return -EINVAL;
	if (!of_property_read_string(chosen, "bootargs", &args)) {
		for (i = 0; i < ARRAY_SIZE(markers); i++) {
			marker = markers[i]; length = strlen(marker);
			for (p = args; (p = strstr(p, marker)); p++)
				if ((p == args || p[-1] == ' ') &&
				    (!p[length] || p[length] == ' '))
					ram = true;
		}
	}
	of_node_put(chosen);
	ret = boot_mode_gate(ram);
	if (ret)
		return ret;
	for (np = of_find_all_nodes(NULL); np; np = of_find_all_nodes(np)) {
		if (of_device_is_compatible(np, "mediatek,mt8183-camisp")) {
			of_node_put(np);
			return -EBUSY;
		}
		for (i = 0; i < ARRAY_SIZE(mt8183_p1_legacy_exclusive); i++)
			if (of_device_is_compatible(np, mt8183_p1_legacy_exclusive[i])) {
				of_node_put(np);
				return -EBUSY;
			}
		if (of_device_is_compatible(np, COMPAT) ||
		    of_device_is_compatible(np, MT8183_P1_LEGACY_COMPAT))
			count++;
	}
	if (count != 1)
		return -EINVAL;
	np = of_find_node_by_path(NODE_PATH);
	if (!np) np = of_find_node_by_path(MT8183_P1_LEGACY_PATH);
	ret = validate_node(np);
	if (ret)
		of_node_put(np);
	else
		test_node = np;
	return ret;
}

static int reject(struct device *dev, const char *reason, int error)
{
	dev_err(dev, "composer test refused: %s (%d)\n", reason, error);
	return error;
}

static bool range32(u64 address, size_t size)
{
	return size && address <= U32_MAX && size - 1 <= U32_MAX - address;
}

static int validate_scp_pool(struct device *scp_dev, struct reserved_mem **pool)
{
	struct device_node *np, *memory;
	struct reserved_mem *rmem;
	bool bad;

	if (!device_is_bound(scp_dev) || !scp_dev->driver ||
	    strcmp(scp_dev->driver->name, "mtk-scp"))
		return -ENODEV;
	if (!of_device_is_compatible(scp_dev->of_node, "mediatek,mt8183-scp") ||
	    get_dma_ops(scp_dev) || use_dma_iommu(scp_dev) ||
	    iommu_get_domain_for_dev(scp_dev) || scp_dev->dma_range_map ||
	    dev_is_dma_coherent(scp_dev) || !scp_dev->dma_mask)
		return -EINVAL;
	for (np = of_node_get(scp_dev->of_node); np; np = of_get_next_parent(np)) {
		if (of_find_property(np, "dma-ranges", NULL) ||
		    of_find_property(np, "dma-coherent", NULL)) {
			of_node_put(np);
			return -EINVAL;
		}
	}
	if (of_count_phandle_with_args(scp_dev->of_node, "memory-region", NULL) != 1)
		return -EINVAL;
	memory = of_parse_phandle(scp_dev->of_node, "memory-region", 0);
	if (!memory)
		return -EINVAL;
	bad = !of_device_is_compatible(memory, "shared-dma-pool") ||
	      !of_property_read_bool(memory, "no-map") ||
	      of_property_read_bool(memory, "reusable") ||
	      of_property_read_bool(memory, "linux,cma-default") ||
	      of_property_read_bool(memory, "linux,dma-default");
	rmem = of_reserved_mem_lookup(memory);
	of_node_put(memory);
	if (bad || !rmem || rmem->base != POOL_BASE || rmem->size != POOL_SIZE ||
	    !rmem->ops || !rmem->priv || !scp_dev->dma_mem ||
	    scp_dev->dma_mem != rmem->priv)
		return -EINVAL;
	/* Fixed coherent.c assigns the same WC pool object to both pointers. */
	*pool = rmem;
	return 0;
}

/* No firmware notification: explicit release must precede core link cleanup. */
static void composer_release(struct composer_state *state)
{
	if (state->mapped) {
		dma_unmap_phys(state->cam, state->cam_iova, TEST_SIZE,
			       TEST_DIR, TEST_ATTRS);
		state->mapped = false;
		COUNT(unmaps);
	}
	if (state->cpu) {
		wmb();
		dma_free_coherent(state->scp, TEST_SIZE, state->cpu, state->scp_dma);
		state->cpu = NULL;
		COUNT(frees);
	}
	if (state->scp) {
		put_device(state->scp);
		state->scp = NULL;
	}
}

static int composer_acquire(struct composer_state *state, struct reserved_mem *pool)
{
	struct device *cam = state->cam, *scp = state->scp;
	struct iommu_domain *domain;
	struct mtk_isp_scp_p1_cmd prepared;
	size_t offset;
	int ret;

	ret = dma_set_mask_and_coherent(cam, DMA_BIT_MASK(32));
	if (ret)
		return reject(cam, "32-bit CAM DMA mask", ret);
	domain = iommu_get_domain_for_dev(cam);
	if (!use_dma_iommu(cam) || get_dma_ops(cam) ||
	    dev_is_dma_coherent(cam) || !domain ||
	    domain->type != IOMMU_DOMAIN_DMA ||
	    domain->cookie_type != IOMMU_COOKIE_DMA_IOVA || !domain->iova_cookie ||
	    domain->geometry.aperture_start ||
	    domain->geometry.aperture_end != CAM_APERTURE_END ||
	    !domain->geometry.force_aperture ||
	    !domain->pgsize_bitmap || __ffs(domain->pgsize_bitmap) != PAGE_SHIFT)
		return reject(cam, "CAM strict DMA-IOMMU backend/aperture/granule", -EINVAL);

	state->cpu = dma_alloc_coherent(scp, TEST_SIZE, &state->scp_dma, GFP_KERNEL);
	if (!state->cpu)
		return -ENOMEM;
	COUNT(allocations);
	state->physical = dma_to_phys(scp, state->scp_dma);
	if (!is_vmalloc_addr(state->cpu) || !IS_ALIGNED(state->physical, TEST_SIZE) ||
	    state->physical < pool->base ||
	    state->physical - pool->base > pool->size - TEST_SIZE ||
	    phys_to_dma(scp, state->physical) != state->scp_dma ||
	    !range32(state->scp_dma, TEST_SIZE) ||
	    state->scp_dma + TEST_SIZE - 1 > scp->coherent_dma_mask ||
	    (scp->bus_dma_limit && state->scp_dma + TEST_SIZE - 1 > scp->bus_dma_limit))
		return reject(cam, "SCP pool address/alias/mask validation", -ERANGE);
	wmb();
	state->cam_iova = dma_map_phys(cam, state->physical, TEST_SIZE,
				       TEST_DIR, TEST_ATTRS);
	if (dma_mapping_error(cam, state->cam_iova))
		return -ENOMEM;
	state->mapped = true;
	COUNT(maps);
	if (!range32(state->cam_iova, TEST_SIZE) ||
	    !IS_ALIGNED(state->cam_iova, PAGE_SIZE) ||
	    state->cam_iova > domain->geometry.aperture_end ||
	    TEST_SIZE - 1 > domain->geometry.aperture_end - state->cam_iova)
		return reject(cam, "CAM IOVA range/alignment", -ERANGE);
	for (offset = 0; offset < TEST_SIZE; offset += PAGE_SIZE) {
		if (iommu_iova_to_phys(domain, state->cam_iova + offset) !=
		    state->physical + offset)
			return reject(cam, "IOVA to physical page mismatch", -EIO);
	}
	state->mapped_pages = TEST_SIZE / PAGE_SIZE;
	/* Encode locally to exercise the fixed ABI. Never publish or send it. */
	return mt8183_p1_encode_init(&prepared, state->scp_dma, state->cam_iova);
}

static irqreturn_t unused_irq_handler(int irq, void *data)
{
	struct resource_state *resources = data;
	struct integrated_state *state = container_of(resources, struct integrated_state, resources);

	atomic_inc(&irq_calls);
	/* Diagnostic counts do not cap a live stream. The actual IRQ parser
	 * validates sequence, DMA/CQ ownership and status on every event. */
	atomic_inc(&state->session_irq_calls);
	mt8183_p1_hw_irq(&state->hw);
	if (READ_ONCE(state->hw.error)) {
		if (atomic_xchg(&state->irq_enabled, 0)) disable_irq_nosync(irq);
		if (smp_load_acquire(&state->workers_active))
			dpw_request_stop(&state->workers, READ_ONCE(state->hw.error));
	}
	return IRQ_HANDLED;
}

static int resources_resume(struct device *dev)
{
	struct integrated_state *state = dev_get_drvdata(dev);
	struct resource_state *s = &state->resources;
	int ret = clk_bulk_prepare_enable(2, s->clocks);

	if (ret)
		return ret; /* bulk helper reverses any partial enable. */
	s->clocks_on = true;
	COUNT(resumes);
	return 0;
}

static int resources_suspend(struct device *dev)
{
	struct integrated_state *state = dev_get_drvdata(dev);
	struct resource_state *s = &state->resources;

	if (s->clocks_on) {
		clk_bulk_disable_unprepare(2, s->clocks);
		s->clocks_on = false;
		COUNT(suspends);
	}
	return 0;
}
/* Standard system-PM admission. Streaming remains a userspace session:
 * refuse sleep while DMA/firmware/queue ownership is live, rather than
 * pretending that a frozen application has performed STREAMOFF.
 */
static int video_retire(struct integrated_state *s);
static int video_initial_idle(struct integrated_state *s);

static int resources_system_prepare(struct device *dev)
{
 struct integrated_state *state = dev_get_drvdata(dev);
 bool busy;
 int ret = 0;
 if (!state || !state->video.queue_initialized)
  return 0;
 if (!mutex_trylock(&state->video.lock))
  return -EBUSY;
 if (!mutex_trylock(&state->control_lock)) {
  mutex_unlock(&state->video.lock);
  return -EBUSY;
 }
 /* An unpublished probe allocation can retire only after driver binding,
  * when genpd can actually finish CAM poweroff. This runs in the pre-device
  * PM notifier, before runtime PM is disabled for system suspend. */
 if (!state->capture_attempted && state->hw.initialized &&
     !state->video.running && !state->stream_module_ref) {
  ret = video_initial_idle(state);
  if (ret) {
   mutex_unlock(&state->control_lock);
   mutex_unlock(&state->video.lock);
   return ret;
  }
 }
 busy = state->video.running || state->stream_module_ref ||
        state->workers_active || state->scp_owned || state->sensor_on ||
        state->seninf_on || state->resources.pm_ref ||
        state->resources.clocks_on || atomic_read(&state->irq_enabled) ||
        state->capture_error || state->input_error || state->transport_failed;
 /* Retire while the normal STOP proof is still valid. System genpd
  * PRE_ON invalidates it; no old DMA owner may require that proof on wake. */
 if (!busy && state->capture_attempted) {
  ret = video_retire(state);
  if (!ret) {
   state->cycle_verified = false; /* Require a fresh real power cycle on wake. */
   dev_info(dev, "P1_SYSTEM_SLEEP_SESSION_RETIRED epoch=%llu\n",
            (unsigned long long)state->session_epoch);
  } else {
   state->capture_error = ret;
   state->transport_failed = true;
   if (!state->stream_module_ref && try_module_get(THIS_MODULE))
    state->stream_module_ref = true;
  }
 }
 if (!ret && !busy)
  state->system_sleep_pending = true;
 mutex_unlock(&state->control_lock);
 mutex_unlock(&state->video.lock);
 if (ret)
  return ret;
 if (busy) {
  dev_info(dev, "P1_SYSTEM_SLEEP_BUSY: close camera before suspend\n");
  return -EBUSY;
 }
 return 0;
}
/* PM notifiers run before device_prepare disables runtime PM on children.
 * Retire the completed session there using the unchanged normal OFF proof;
 * hold admission closed until PM_POST_SUSPEND (including aborted sleep).
 * The device .prepare repeats the guard without re-retiring old DMA.
 */
static int resources_system_notify(struct notifier_block *nb,
                                   unsigned long event, void *unused)
{
 struct integrated_state *state = container_of(nb, struct integrated_state,
                                              system_notifier);
 int ret;
 (void)unused;
 if (event == PM_SUSPEND_PREPARE) {
  ret = resources_system_prepare(state->resources.dev);
  return notifier_from_errno(ret);
 }
 if (event == PM_POST_SUSPEND) {
  mutex_lock(&state->video.lock);
  state->system_sleep_pending = false;
  mutex_unlock(&state->video.lock);
 }
 return NOTIFY_OK;
}
static const struct dev_pm_ops resources_pm_ops = {
	.prepare = resources_system_prepare,
	.suspend = pm_runtime_force_suspend,
	.resume = pm_runtime_force_resume,
	.runtime_resume = resources_resume,
	.runtime_suspend = resources_suspend,
};

static int resources_release(struct resource_state *s)
{
	int ret = 0;

	/* Probe unwind has a disabled IRQ; explicit stop already disabled/drained it. */
	if (s->irq_owned) {
		synchronize_irq(s->irq);
		free_irq(s->irq, s);
		s->irq_owned = false;
		COUNT(irq_frees);
	}
	if (s->pm_ref) {
		ret = pm_runtime_put_sync_suspend(s->dev);
		s->pm_ref = false; /* put decrements even on suspend failure. */
		COUNT(pm_puts);
	}
	if (s->pm_enabled) {
		pm_runtime_disable(s->dev); /* drain callbacks before state is freed. */
		s->pm_enabled = false;
		COUNT(pm_disables);
	}
	if (ret >= 0 && s->clocks_on)
		ret = -EIO; /* A nonnegative put did not complete our clock suspend. */
	if (ret < 0) {
		COUNT(cleanup_errors);
		dev_err(s->dev, "runtime suspend error %d; disabling owned clocks after PM barrier\n", ret);
	}
	/* Probe unwind is unpublished; a published session passed the OFF gate. */
	if (s->clocks_on)
		resources_suspend(s->dev);
	if (s->clocks_owned) {
		clk_bulk_put(2, s->clocks);
		s->clocks_owned = false;
		COUNT(clk_puts);
	}
	if (s->base) {
		iounmap(s->base);
		s->base = NULL;
		COUNT(munmaps);
	}
	if (s->region_owned) {
		release_mem_region(CAM_BASE, CAM_SIZE);
		s->region_owned = false;
		COUNT(regions_freed);
	}
	/* Driver core subsequently detaches the shared PM domain. Do not claim
	 * runtime-suspended means the hardware domain is powered off.
	 */
	return ret < 0 ? ret : 0;
}

static bool is_compatible_fwnode(struct fwnode_handle *fn, const char *compat)
{
	return is_of_node(fn) && of_device_is_available(to_of_node(fn)) &&
	       of_device_is_compatible(to_of_node(fn), compat);
}

/* The experiment DT port and public RAW sink are both 0; FW pad ABI is separate. */
static int graph_add_connection(struct mt8183_p1_graph *graph)
{
	struct fwnode_handle *local = NULL, *remote = NULL, *back = NULL;
	struct fwnode_handle *input = NULL, *sensor_ep = NULL;
	struct fwnode_endpoint base = { 0 };
	struct v4l2_async_connection *asc;
	int ret = -EINVAL;

	if (fwnode_graph_get_endpoint_count(dev_fwnode(graph->dev), 0) != 1)
		return -EINVAL;
	local = fwnode_graph_get_next_endpoint(dev_fwnode(graph->dev), NULL);
	if (!local || fwnode_graph_parse_endpoint(local, &base) ||
	    base.port != P1_DT_PORT || base.id)
		goto out;
	remote = fwnode_graph_get_remote_endpoint(local);
	if (!remote || fwnode_graph_parse_endpoint(remote, &base) ||
	    base.port != SENINF_SOURCE_PAD || base.id)
		goto out;
	back = fwnode_graph_get_remote_endpoint(remote);
	if (back != local)
		goto out;
	graph->seninf_fwnode = fwnode_graph_get_port_parent(remote);
	if (!is_compatible_fwnode(graph->seninf_fwnode, "mediatek,mt8183-seninf"))
		goto out;

	for (unsigned int port = 0; port < 2; port++) {
		input = fwnode_graph_get_endpoint_by_id(graph->seninf_fwnode, port, 0, 0);
		if (!input) goto out;
		sensor_ep = fwnode_graph_get_remote_endpoint(input);
		if (!sensor_ep || fwnode_graph_parse_endpoint(sensor_ep, &base) || base.port || base.id) goto out;
		fwnode_handle_put(back); back = fwnode_graph_get_remote_endpoint(sensor_ep);
		if (back != input) goto out;
		graph->sensor_fwnode[port] = fwnode_graph_get_port_parent(sensor_ep);
		if (!is_compatible_fwnode(graph->sensor_fwnode[port], port ? "ovti,ov02a10" : "ovti,ov8856")) goto out;
		fwnode_handle_put(sensor_ep); sensor_ep = NULL;
		fwnode_handle_put(input); input = NULL;
	}
	asc = v4l2_async_nf_add_fwnode_remote(&graph->notifier, local,
					  struct v4l2_async_connection);
	ret = IS_ERR(asc) ? PTR_ERR(asc) : 0;
out:
	fwnode_handle_put(sensor_ep);
	fwnode_handle_put(input);
	fwnode_handle_put(back);
	fwnode_handle_put(remote);
	fwnode_handle_put(local);
	/* Stored device fwnodes are released by graph_cleanup(), including error. */
	return ret;
}

/* This formatter does no independent DMA/stream transition. Capture owns
 * sensor/SENINF/P1 ordering through VB2, including the proven stop gate. */
static int graph_s_stream(struct v4l2_subdev *sd, int enable)
{
 (void)sd;(void)enable;return 0;
}
/* The capture frontend preserves CFA order; no colour conversion here. */
static const u32 dcv_bayer_codes[] = { MEDIA_BUS_FMT_SBGGR10_1X10,
 MEDIA_BUS_FMT_SGBRG10_1X10, MEDIA_BUS_FMT_SGRBG10_1X10, MEDIA_BUS_FMT_SRGGB10_1X10 };
static int dcv_bayer_index(u32 code)
{ unsigned int i; for(i=0;i<4;i++)if(dcv_bayer_codes[i]==code)return i;return -EINVAL; }
static void graph_format(struct v4l2_mbus_framefmt *f)
{
 memset(f,0,sizeof(*f));f->width=DCV_WIDTH;f->height=DCV_HEIGHT;
 f->code=MEDIA_BUS_FMT_SRGGB10_1X10;f->field=V4L2_FIELD_NONE;
 f->colorspace=V4L2_COLORSPACE_RAW;f->xfer_func=V4L2_XFER_FUNC_NONE;
 f->quantization=V4L2_QUANTIZATION_FULL_RANGE;
}
static int graph_init_state(struct v4l2_subdev *sd,struct v4l2_subdev_state *state)
{
 unsigned int pad;(void)sd;
 for(pad=0;pad<P1_NUM_PADS;pad++)graph_format(v4l2_subdev_state_get_format(state,pad));
 return 0;
}
static int graph_enum_code(struct v4l2_subdev *sd,struct v4l2_subdev_state *state,struct v4l2_subdev_mbus_code_enum *code)
{
 (void)sd;(void)state;
 if(code->pad>=P1_NUM_PADS||code->index>=4||code->stream)return -EINVAL;
 code->code=dcv_bayer_codes[code->index];return 0;
}
static int graph_enum_size(struct v4l2_subdev *sd,struct v4l2_subdev_state *state,struct v4l2_subdev_frame_size_enum *size)
{
 (void)sd;(void)state;
 if(size->pad>=P1_NUM_PADS||size->index>2||size->stream||dcv_bayer_index(size->code)<0)return -EINVAL;
 size->min_width=size->max_width=size->index==2?3264:size->index?1632:1600;size->min_height=size->max_height=size->index==2?2448:size->index?1224:1200;return 0;
}
static int graph_get_fmt(struct v4l2_subdev *sd,struct v4l2_subdev_state *state,struct v4l2_subdev_format *fmt)
{
 (void)sd;
 if(fmt->pad>=P1_NUM_PADS||fmt->stream)return -EINVAL;
 fmt->format=*v4l2_subdev_state_get_format(state,fmt->pad);return 0;
}
static int graph_set_fmt(struct v4l2_subdev *sd,struct v4l2_subdev_state *state,struct v4l2_subdev_format *fmt)
{
 struct mt8183_p1_graph *g=container_of(sd,struct mt8183_p1_graph,subdev);
 struct integrated_state *s=container_of(g,struct integrated_state,graph);
 unsigned int pad;
 if(fmt->pad>=P1_NUM_PADS||fmt->stream)return -EINVAL;
 if(fmt->which==V4L2_SUBDEV_FORMAT_ACTIVE&&READ_ONCE(s->video.running))return -EBUSY;
 u32 code=fmt->format.code,w=fmt->format.width,h=fmt->format.height;graph_format(&fmt->format);
 if(w<=1600&&h<=1200){fmt->format.width=1600;fmt->format.height=1200;}else if(w>1632||h>1224){fmt->format.width=3264;fmt->format.height=2448;}
 if(dcv_bayer_index(code)>=0)fmt->format.code=code;
 /* Fixed-size identity formatter: TRY/ACTIVE state propagates both pads. */
 for(pad=0;pad<P1_NUM_PADS;pad++)*v4l2_subdev_state_get_format(state,pad)=fmt->format;
 return 0;
}
static int graph_subscribe_event(struct v4l2_subdev *sd,struct v4l2_fh *fh,
                                 struct v4l2_event_subscription *sub)
{
 (void)sd;
 if(sub->type!=V4L2_EVENT_FRAME_SYNC||sub->id)return -EINVAL;
 return v4l2_event_subscribe(fh,sub,8,NULL);
}
static const struct v4l2_subdev_core_ops graph_core_ops={
 .subscribe_event=graph_subscribe_event,.unsubscribe_event=v4l2_event_subdev_unsubscribe,
};
static const struct v4l2_subdev_video_ops graph_video_ops={.s_stream=graph_s_stream};
static const struct v4l2_subdev_pad_ops graph_pad_ops={.enum_mbus_code=graph_enum_code,.enum_frame_size=graph_enum_size,.get_fmt=graph_get_fmt,.set_fmt=graph_set_fmt};
static const struct v4l2_subdev_internal_ops graph_internal_ops={.init_state=graph_init_state};
static const struct media_entity_operations graph_entity_ops={.link_validate=v4l2_subdev_link_validate};
static const struct v4l2_subdev_ops graph_subdev_ops={.core=&graph_core_ops,.video=&graph_video_ops,.pad=&graph_pad_ops};
static void graph_nodes_release(struct v4l2_device *v4l2)
{
 struct mt8183_p1_graph *g=container_of(v4l2,struct mt8183_p1_graph,v4l2);
 complete(&g->nodes_released);
}
/* Standard link setup is permitted while idle. Prevent topology changes
 * during admission/streaming. Userspace must enable the sensor link before
 * STREAMON; capture preparation never changes an active pipeline topology. */
static int graph_link_notify(struct media_link *link,u32 flags,unsigned int notification)
{
 struct mt8183_p1_graph *g=container_of(link->graph_obj.mdev,struct mt8183_p1_graph,media);
 struct integrated_state *s=container_of(g,struct integrated_state,graph);
 if(notification!=MEDIA_DEV_NOTIFY_PRE_LINK_CH)return 0;
 (void)flags;
 return READ_ONCE(s->video.running)?-EBUSY:0;
}

static const struct media_device_ops graph_media_ops = {
	.link_notify = graph_link_notify,
};

static int graph_bound(struct v4l2_async_notifier *notifier,
		       struct v4l2_subdev *sd, struct v4l2_async_connection *asc)
{
	struct mt8183_p1_graph *graph =
		container_of(notifier, struct mt8183_p1_graph, notifier);

	if (graph->seninf || !sd->dev ||
	    dev_fwnode(sd->dev) != graph->seninf_fwnode ||
	    sd->entity.function != MEDIA_ENT_F_VID_IF_BRIDGE ||
	    sd->entity.num_pads <= SENINF_SOURCE_PAD ||
	    !(sd->entity.pads[SENINF_SOURCE_PAD].flags & MEDIA_PAD_FL_SOURCE) ||
	    !(sd->entity.pads[SENINF_SENSOR_PAD].flags & MEDIA_PAD_FL_SINK))
		return -EINVAL;
	graph->seninf = sd;
	dev_info(graph->dev, "SENINF bound; waiting for nested sensor notifier\n");
	return 0;
}

static void graph_unbind(struct v4l2_async_notifier *notifier,
			 struct v4l2_subdev *sd, struct v4l2_async_connection *asc)
{
	struct mt8183_p1_graph *graph =
		container_of(notifier, struct mt8183_p1_graph, notifier);

	/* Async core unregisters the entity and removes its links immediately next. */
	graph->seninf = NULL;
	graph->complete = false;
	dev_info(graph->dev, "SENINF unbound; graph incomplete\n");
}

/* Caller holds graph_mutex; async core also serializes notifier callbacks. */
static bool graph_has_rear_sensor(struct mt8183_p1_graph *graph)
{
 struct media_link *link;unsigned int found=0;
 list_for_each_entry(link,&graph->seninf->entity.links,list) {
  struct v4l2_subdev *sensor;
  if((link->flags&MEDIA_LNK_FL_LINK_TYPE)!=MEDIA_LNK_FL_DATA_LINK || link->sink->entity!=&graph->seninf->entity || link->sink->index>=2)continue;
  if(!is_media_entity_v4l2_subdev(link->source->entity)||link->source->entity->function!=MEDIA_ENT_F_CAM_SENSOR||link->source->index)return false;
  sensor=media_entity_to_v4l2_subdev(link->source->entity);
  if(!sensor->dev||dev_fwnode(sensor->dev)!=graph->sensor_fwnode[link->sink->index]||(found&BIT(link->sink->index)))return false;
  found|=BIT(link->sink->index);
 }
 return found==3;
}

static int graph_complete(struct v4l2_async_notifier *notifier)
{
	struct mt8183_p1_graph *graph =
		container_of(notifier, struct mt8183_p1_graph, notifier);
	struct media_link *link;
	int ret = 0;

	if (!graph->seninf)
		return -ENODEV;
	mutex_lock(&graph->media.graph_mutex);
	if (!graph_has_rear_sensor(graph)) {
		ret = -ENOLINK;
		goto out;
	}
	/* Sensor rebind can invoke complete again while this link still exists. */
	link = media_entity_find_link(&graph->seninf->entity.pads[SENINF_SOURCE_PAD],
				      &graph->pads[P1_SINK_PAD]);
	if (link) {
		if (link->flags != GRAPH_LINK_FLAGS)
			ret = -EINVAL;
	} else {
		ret = media_create_pad_link(&graph->seninf->entity, SENINF_SOURCE_PAD,
					    &graph->subdev.entity, P1_SINK_PAD,
					    GRAPH_LINK_FLAGS);
	}
out:
	mutex_unlock(&graph->media.graph_mutex);
	if (!ret) {
		graph->complete = true;
		COUNT(graph_completes);
		dev_info(graph->dev,
			 "graph complete: dual sensors -> SENINF:0/1; SENINF:4 -> P1:0; RAW capture node published after probe\n");
	}
	return ret;
}

static const struct v4l2_async_notifier_operations graph_async_ops = {
	.bound = graph_bound,
	.unbind = graph_unbind,
	.complete = graph_complete,
};

static void graph_cleanup(struct mt8183_p1_graph *graph)
{
	if (graph->sensor_pinned) {
		module_put(graph->sensor->owner);
		put_device(graph->sensor->dev);
		graph->sensor_pinned = false;
	}
	if (graph->seninf_pinned) {
		module_put(graph->seninf->owner);
		put_device(graph->seninf->dev);
		graph->seninf_pinned = false;
	}
	/* Stop callbacks before media unregister removes all registered entities. */
	if (graph->notifier_registered) {
		v4l2_async_nf_unregister(&graph->notifier);
		graph->notifier_registered = false;
		COUNT(notifier_unregs);
	}
	if (graph->notifier_initialized) {
		v4l2_async_nf_cleanup(&graph->notifier);
		graph->notifier_initialized = false;
		COUNT(notifier_cleanups);
	}
	if (graph->media_registered) {
		media_device_unregister(&graph->media);
		graph->media_registered = false;
		COUNT(media_unregs);
	}
	if (graph->subdev_registered) {
		v4l2_device_unregister_subdev(&graph->subdev);
		graph->subdev_registered = false;
		COUNT(subdev_unregs);
	}
	if (graph->v4l2_registered) {
		v4l2_device_unregister(&graph->v4l2);
		graph->v4l2_registered = false;
		COUNT(v4l2_unregs);
		/* Node core references cover successful and racing failed opens.
		 * No embedded state/queue/pad storage can be freed before this. */
		v4l2_device_put(&graph->v4l2);
		wait_for_completion(&graph->nodes_released);
	}
	if (graph->subdev_finalized) {
		v4l2_subdev_cleanup(&graph->subdev);
		graph->subdev_finalized = false;
	}
	if (graph->entity_initialized) {
		media_entity_cleanup(&graph->subdev.entity);
		graph->entity_initialized = false;
		COUNT(entity_cleanups);
	}
	if (graph->media_initialized) {
		media_device_cleanup(&graph->media);
		graph->media_initialized = false;
		COUNT(media_cleanups);
	}
	fwnode_handle_put(graph->sensor_fwnode[0]);
	fwnode_handle_put(graph->sensor_fwnode[1]);
	fwnode_handle_put(graph->seninf_fwnode);
	graph->sensor_fwnode[0] = graph->sensor_fwnode[1] = NULL;
	graph->seninf_fwnode = NULL;
}

static int graph_register(struct mt8183_p1_graph *graph)
{
	struct device *dev = graph->dev;
	unsigned int i;
	int ret;

	graph->media.dev = dev;
	strscpy(graph->media.model, "MT8183 P1 RAW", sizeof(graph->media.model));
	media_device_init(&graph->media);
	graph->media_initialized = true;
	COUNT(media_inits);
	graph->media.ops = &graph_media_ops;
	init_completion(&graph->nodes_released);
	graph->v4l2.release = graph_nodes_release;
	graph->v4l2.mdev = &graph->media;
	ret = v4l2_device_register(dev, &graph->v4l2);
	if (ret)
		goto fail;
	graph->v4l2_registered = true;
	COUNT(v4l2_regs);
	v4l2_subdev_init(&graph->subdev, &graph_subdev_ops);
	graph->subdev.owner = THIS_MODULE;
	graph->subdev.dev = dev;
	strscpy(graph->subdev.name, "mtk-cam-p1-raw", sizeof(graph->subdev.name));
	graph->subdev.entity.function = MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER;
	graph->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE|V4L2_SUBDEV_FL_HAS_EVENTS;
	graph->subdev.internal_ops = &graph_internal_ops;
	graph->subdev.entity.ops = &graph_entity_ops;
	/* These are public MC pad identities, separate from the firmware ABI. */
	for (i = 0; i < P1_NUM_PADS; i++)
		graph->pads[i].flags = i == P1_SINK_PAD ?
			(MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT) : MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&graph->subdev.entity, P1_NUM_PADS, graph->pads);
	if (ret)
		goto fail;
	graph->entity_initialized = true;
	COUNT(entity_inits);
	ret = v4l2_subdev_init_finalize(&graph->subdev);
	if (ret) goto fail;
	graph->subdev_finalized = true;
	ret = v4l2_device_register_subdev(&graph->v4l2, &graph->subdev);
	if (ret)
		goto fail;
	graph->subdev_registered = true;
	COUNT(subdev_regs);
	v4l2_async_nf_init(&graph->notifier, &graph->v4l2);
	graph->notifier_initialized = true;
	COUNT(notifier_inits);
	graph->notifier.ops = &graph_async_ops;
	ret = graph_add_connection(graph);
	if (ret)
		goto fail;
	ret = v4l2_async_nf_register(&graph->notifier);
	if (ret)
		goto fail;
	graph->notifier_registered = true;
	COUNT(notifier_regs);
	/* Experiment load order requires the complete fixed sensor path. No
	 * late notifier may publish new nodes after admission starts. */
	if (!graph->complete) { ret = -EPROBE_DEFER; goto fail; }
	return 0;
fail:
	return ret;
}

static int resources_acquire(struct platform_device *pdev, struct resource_state *s)
{
	struct device *dev = &pdev->dev;
	struct irq_data *data;
	int ret;

	if (!request_mem_region(CAM_BASE, CAM_SIZE, dev_name(dev))) {
		ret = -EBUSY;
		goto out;
	}
	s->region_owned = true;
	COUNT(regions);
	s->base = ioremap(CAM_BASE, CAM_SIZE);
	if (!s->base) {
		ret = -ENOMEM;
		goto out;
	}
	COUNT(mmaps);
	ret = clk_bulk_get(dev, 2, s->clocks);
	if (ret)
		goto out;
	s->clocks_owned = true;
	COUNT(clk_gets);
	s->irq = platform_get_irq(pdev, 0);
	if (s->irq < 0) { ret = s->irq; goto out; }
	data = irq_get_irq_data(s->irq);
	if (!data || data->hwirq != 255 || !data->parent_data ||
	    data->parent_data->hwirq != 287 ||
	    !data->chip || !data->chip->name || strcmp(data->chip->name, "MT_SYSIRQ") ||
	    !data->parent_data->chip || !data->parent_data->chip->name ||
	    strcmp(data->parent_data->chip->name, "GICv3") ||
	    irq_get_trigger_type(s->irq) != IRQ_TYPE_LEVEL_LOW) {
		ret = -EINVAL;
		goto out;
	}
	ret = request_irq(s->irq, unused_irq_handler, IRQF_NO_AUTOEN, dev_name(dev), s);
	if (ret)
		goto out;
	s->irq_owned = true;
	COUNT(irq_requests);
	if (!irqd_irq_disabled(data)) { ret = -EINVAL; goto out; }
	if (dev->power.disable_depth != 1 || atomic_read(&dev->power.usage_count) ||
	    !pm_runtime_status_suspended(dev)) {
		ret = -EINVAL;
		goto out;
	}
	ret = pm_runtime_set_suspended(dev);
	if (ret)
		goto out;
	pm_runtime_enable(dev);
	s->pm_enabled = true;
	COUNT(pm_enables);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto out;
	s->pm_ref = true;
	COUNT(pm_gets);
	if (!s->clocks_on) { ret = -EIO; goto out; }
	return 0;
out:
	return ret;
}

static int integrated_release(struct integrated_state *state)
{
	struct mt8183_p1_hw_snapshot h;
	int ret;

	if (state->workers.initialized && !state->workers.drained) return -EBUSY;
	/* Parent control mutex and drained callbacks exclude a new pending frame. */
	mt8183_p1_hw_observe(&state->hw, &h);
	if (h.frame_pending || h.refill_pending || h.copy_pending ||
	    (h.published && !state->stop_verified))
		return -EBUSY;
	ret = mt8183_p1_stop_gate_cleanup(&state->stop_gate);
	if (ret)
		return ret;
	/* No published allocation is freed without the parent's raw OFF gate. */
	if (state->hw.adapter.bound && state->inputs_idle)
		mt8183_p1_hw_inputs_stopped(&state->hw, true);
	ret = mt8183_p1_hw_release(&state->hw, state->stop_verified);
	if (ret)
		return ret;

	/* Withdraw userspace/notifiers first, before clock or mapping release. */
	dcv_unregister(&state->video);
	graph_cleanup(&state->graph);
	dcv_cleanup(&state->video);
	if (state->scp_api) { scp_put(state->scp_api); state->scp_api = NULL; }
	ret = resources_release(&state->resources);
	composer_release(&state->composer);
	if (state->smi_ref) {
		int smi_ret = pm_runtime_put_sync_suspend(state->smi_dev);
		state->smi_ref = false;
		if (smi_ret < 0 && !ret)
			ret = smi_ret;
	}
	if (state->smi_dev) { put_device(state->smi_dev); state->smi_dev = NULL; }
	return ret;
}

/* Receive snapshots bytes, updates the bounded frame queue and wakes waiters. */
static void stream_ipi(void *data, unsigned int len, void *priv)
{
	struct ipi_channel *ch = priv;
	struct integrated_state *state = ch->state;
	unsigned long flags;

	spin_lock_irqsave(&state->rx_lock, flags);
	ch->count++;
	ch->len = len;
	memset(ch->bytes, 0, sizeof(ch->bytes));
	if (data && len <= sizeof(ch->bytes))
		memcpy(ch->bytes, data, len);
	spin_unlock_irqrestore(&state->rx_lock, flags);
	mt8183_p1_hw_rx(&state->hw, ch->id, data, len);
	complete(&ch->reply);
	/* hw_rx has released its spinlock before close reacquires it. */
	if (READ_ONCE(state->hw.error) && smp_load_acquire(&state->workers_active))
		dpw_request_stop(&state->workers, READ_ONCE(state->hw.error));
}

static bool rproc_is(struct integrated_state *state, int power, int expected)
{
	bool ok;

	mutex_lock(&state->rproc->lock);
	ok = atomic_read(&state->rproc->power) == power &&
		state->rproc->state == expected && state->rproc->recovery_disabled;
	mutex_unlock(&state->rproc->lock);
	return ok;
}

/* One command outstanding, no retry after uncertainty. A reply/echo is an
 * observation, never proof of semantic success, HW idle, or safe release.
 */
static int send_cmd(struct integrated_state *state, struct mtk_isp_scp_p1_cmd *cmd)
{
	struct ipi_channel *ch = &state->channel[0];
	unsigned long flags;
	u32 before;
	bool echo = false;
	int ret;

	if (!rproc_is(state, 1, RPROC_RUNNING)) {
		ret = -EBUSY;
		goto failed;
	}
	ret = READ_ONCE(state->hw.error);
	if (ret)
		goto failed;
	spin_lock_irqsave(&state->rx_lock, flags);
	before = ch->count;
	reinit_completion(&ch->reply);
	spin_unlock_irqrestore(&state->rx_lock, flags);
	/* Conservative publication precedes doorbell; even send failure holds RAM. */
	state->fw_exposed = true;
	state->commands++;
	wmb();
	ret = scp_ipi_send(state->scp_api, ch->id, cmd, sizeof(*cmd), 0);
	if (!ret && !wait_for_completion_timeout(&ch->reply, msecs_to_jiffies(1500)))
		ret = -ETIMEDOUT;
	spin_lock_irqsave(&state->rx_lock, flags);
	echo = ch->count == before + 1 && ch->len >= MT8183_P1_ACK_MIN_BYTES && ch->len <= MT8183_P1_RX_MAX_BYTES &&
		ch->bytes[0] == ISP_CMD_ACK && ch->bytes[1] == cmd->cmd_id;
	spin_unlock_irqrestore(&state->rx_lock, flags);
	if (!ret && !echo)
		ret = -EPROTO;
	if (!ret)
		ret = READ_ONCE(state->hw.error);
failed:
	state->send_error = ret;
	if (ret) {
		state->transport_failed = true;
		mt8183_p1_hw_hold(&state->hw, ret);
	}
	dev_info(state->resources.dev, "P1_IPI cmd=%u ret=%d echo=%u commands=%u\n",
		cmd->cmd_id, ret, echo, state->commands);
	return ret;
}

static int capture_set_format(struct v4l2_subdev *sd, unsigned int pad, u32 code, u32 width, u32 height)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE, .pad = pad,
		.format = { .width = width, .height = height,
			.code = code, .field = V4L2_FIELD_NONE },
	};
	/* Reapplying OV8856 ACTIVE format resets its VBLANK/exposure controls.
	 * Preserve the standard control owner when the format already matches. */
	struct v4l2_subdev_format active_fmt = { .which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = pad };
	int ret = v4l2_subdev_call(sd, pad, get_fmt, NULL, &active_fmt);
	if (ret) return ret;
	if (active_fmt.format.width == width && active_fmt.format.height == height &&
	    active_fmt.format.code == code && active_fmt.format.field == V4L2_FIELD_NONE)
		return 0;
	ret = v4l2_subdev_call(sd, pad, set_fmt, NULL, &fmt);

	if (!ret)
		ret = v4l2_subdev_call(sd, pad, get_fmt, NULL, &fmt);
	if (!ret && (fmt.format.width != width || fmt.format.height != height ||
		     fmt.format.code != code ||
		     fmt.format.field != V4L2_FIELD_NONE))
		ret = -EINVAL;
	return ret;
}

/* Controlled single-owner harness: no child unbind or competing PM/control. */
static int capture_prepare(struct integrated_state *state)
{
	struct mt8183_p1_graph *g = &state->graph;
	struct media_link *link, *input = NULL;
	struct v4l2_subdev *sensor;
 struct v4l2_subdev_format fmt = { .which=V4L2_SUBDEV_FORMAT_ACTIVE,.pad=0 };
	int ret = -ENOLINK;

	if (!state->cycle_verified || state->stage || state->fw_exposed ||
	    state->transport_failed || g->prepared || g->sensor_pinned ||
	    !g->seninf || !state->resources.pm_ref)
		return -EPERM;
	mutex_lock(&g->media.graph_mutex);
	if (!graph_has_rear_sensor(g))
		goto unlock;
	list_for_each_entry(link, &g->seninf->entity.links, list)
		if ((link->flags & MEDIA_LNK_FL_LINK_TYPE) == MEDIA_LNK_FL_DATA_LINK &&
		    link->sink->entity == &g->seninf->entity && link->sink->index < 2 && (link->flags & MEDIA_LNK_FL_ENABLED)) {
            if(input) goto unlock;
			input = link;
        }
	if (!input || input->flags != MEDIA_LNK_FL_ENABLED)
		goto unlock;
	sensor = media_entity_to_v4l2_subdev(input->source->entity);
	if (!device_is_bound(sensor->dev) || !device_is_bound(g->seninf->dev))
		goto unlock;
	if (!try_module_get(sensor->owner)) { ret = -ENODEV; goto unlock; }
	get_device(sensor->dev);
	g->sensor = sensor;
	g->sensor_pinned = true;
	if (!try_module_get(g->seninf->owner)) { ret = -ENODEV; goto unlock; }
	get_device(g->seninf->dev);
	g->seninf_pinned = true;
	ret = v4l2_subdev_call(sensor,pad,get_fmt,NULL,&fmt);
 if(ret||dcv_bayer_index(fmt.format.code)<0||fmt.format.width!=state->video.width||fmt.format.height!=state->video.height){if(!ret)ret=-EINVAL;goto unlock;}
 state->sensor_code=fmt.format.code;
 ret = capture_set_format(sensor, 0, state->sensor_code,state->video.width,state->video.height);
	if (!ret) ret = capture_set_format(g->seninf, input->sink->index, state->sensor_code,state->video.width,state->video.height);
	if (!ret) ret = capture_set_format(g->seninf, SENINF_SOURCE_PAD, state->sensor_code,state->video.width,state->video.height);
	if (ret) goto unlock;
	/* The immutable link existed before setup. Invoke its receiver mux setup
	 * explicitly; creation of an enabled link does not call link_setup.
	 */
	ret = media_entity_call(&g->seninf->entity, link_setup,
			       &g->seninf->entity.pads[SENINF_SOURCE_PAD],
			       &g->pads[P1_SINK_PAD], GRAPH_LINK_FLAGS);
	if (ret) goto unlock;
	state->hw.profile.bayer_id = dcv_bayer_index(state->sensor_code);
	g->prepared = true;
	dev_info(g->dev, "P1_CAPTURE_PREPARED width=%u height=%u code=%x sink=%u source=4\n",
		 state->video.width,state->video.height,state->sensor_code,input->sink->index);
unlock:
	mutex_unlock(&g->media.graph_mutex);
	return ret;
}

/* A child's stream-off consumes its own PM reference even on I2C error.
 * Never retry that put. Drain asynchronous sensor PM, then request suspend
 * without acquiring/returning another reference. An I2C error remains visible
 * as input_error; independently verified power-off can still permit teardown.
 */
static int capture_child_idle(struct device *dev)
{
	unsigned long flags;
	bool idle;
	int ret;

	pm_runtime_barrier(dev);
	ret = pm_runtime_suspend(dev);
	spin_lock_irqsave(&dev->power.lock, flags);
	idle = pm_runtime_status_suspended(dev) &&
		!atomic_read(&dev->power.usage_count);
	spin_unlock_irqrestore(&dev->power.lock, flags);
	return idle ? 0 : (ret < 0 ? ret : -EBUSY);
}

static int capture_inputs_stop(struct integrated_state *state)
{
	struct mt8183_p1_graph *g = &state->graph;
	int ret, first = 0;

	if (state->sensor_on) {
		ret = v4l2_subdev_call(g->sensor, video, s_stream, 0);
		state->sensor_on = false;
		if (ret && !state->input_error) state->input_error = ret;
	}
	if (state->seninf_on) {
		ret = v4l2_subdev_call(g->seninf, video, s_stream, 0);
		state->seninf_on = false;
		if (ret && !state->input_error) state->input_error = ret;
	}
	if (g->sensor_pinned) {
		ret = capture_child_idle(g->sensor->dev);
		if (ret) first = ret;
	}
	if (g->seninf_pinned) {
		ret = capture_child_idle(g->seninf->dev);
		if (ret && !first) first = ret;
	}
	if (first && !state->input_error) state->input_error = first;
	state->inputs_idle = !first;
	if (state->input_error) mt8183_p1_hw_hold(&state->hw, state->input_error);
	return first;
}

static int stream_boot_init(struct integrated_state *state)
{
	struct mtk_isp_scp_p1_cmd cmd;
	int ret, i;

	if (!state->graph.prepared || !state->cycle_verified || state->stage || state->fw_exposed ||
	    state->transport_failed || !state->resources.pm_ref ||
	    !state->resources.clocks_on || !state->composer.mapped ||
	    !rproc_is(state, 0, RPROC_OFFLINE))
		return -EPERM;
	ret = mt8183_p1_encode_init(&cmd, state->composer.scp_dma,
				 state->composer.cam_iova);
	if (ret)
		return ret;
	/* The RAM harness has no other ISP consumer. register() does not arbitrate. */
	for (i = 0; i < 2; i++) {
		ret = scp_ipi_register(state->scp_api, state->channel[i].id,
				       stream_ipi, &state->channel[i]);
		if (ret)
			return ret;
		state->ipi_registered[i] = true;
	}
	ret = rproc_boot(state->rproc);
	if (ret) {
		state->transport_failed = true;
		return ret;
	}
	state->scp_owned = true;
	if (!rproc_is(state, 1, RPROC_RUNNING)) {
		state->transport_failed = true;
		return -EBUSY;
	}
	ret = mt8183_p1_hw_publish(&state->hw);
	if (ret) { state->transport_failed = true; return ret; }
	ret = send_cmd(state, &cmd);
	if (!ret)
		state->stage = 1;
	return ret;
}

static int cam_put_and_gate(struct integrated_state *state)
{
	struct resource_state *s = &state->resources;
	int ret;

	if (s->pm_ref) {
		ret = mt8183_p1_stop_gate_arm(&state->stop_gate);
		if (ret)
			return ret;
		ret = pm_runtime_put_sync_suspend(s->dev);
		s->pm_ref = false;
		COUNT(pm_puts);
		if (ret < 0)
			return ret;
	}
	/* Larb supplier idle/poweroff may complete after the consumer callback. */
	ret = mt8183_p1_stop_gate_wait(&state->stop_gate, &state->stop_snapshot);
	if (ret)
		return ret;
	if (s->clocks_on || !pm_runtime_status_suspended(s->dev) ||
	    atomic_read(&s->dev->power.usage_count))
		return -EBUSY;
	return 0;
}

static int stream_cycle(struct integrated_state *state)
{
	struct resource_state *s = &state->resources;
	int ret;

	if (state->fw_exposed || state->stage || state->transport_failed ||
	    !s->pm_ref || !rproc_is(state, 0, RPROC_OFFLINE))
		return -EPERM;
	ret = cam_put_and_gate(state);
	if (ret) {
		state->transport_failed = true;
		return ret;
	}
	state->cycle_verified = true;
	state->cycles++;
	ret = pm_runtime_resume_and_get(s->dev);
	if (ret < 0) {
		state->transport_failed = true;
		return ret;
	}
	s->pm_ref = true;
	COUNT(pm_gets);
	if (!s->clocks_on)
		return -EIO;
	dev_info(s->dev, "P1_CYCLE_VERIFIED count=%u\n", state->cycles);
	return 0;
}

static int stream_stop(struct integrated_state *state)
{
	int ret, i;

	if (state->stopped)
		return -EALREADY;
	state->stopping = true;
	mt8183_p1_hw_stopping(&state->hw);
	/* The SENINF shares CAM's domain. Its reference must be gone first. */
	ret = capture_inputs_stop(state);
	if (ret)
		return ret;
	if (state->resources.irq_owned) {
		if (atomic_xchg(&state->irq_enabled, 0))
			disable_irq(state->resources.irq);
		else
			synchronize_irq(state->resources.irq);
	}
	/* Drain latched status that might not have reached the CPU before masking.
	 * Sensor/receiver are idle, CAM remains held. This is an observation, not
	 * a DMA stop gate. Any late SOF/DONE/error invalidates image publication.
	 */
	if (state->capture_attempted && state->resources.pm_ref &&
	    state->resources.clocks_on)
		mt8183_p1_hw_irq(&state->hw);
	if (state->scp_owned) {
		/* Only our final reference may stop this processor. No ref-draining loop. */
		if (!rproc_is(state, 1, RPROC_RUNNING))
			return -EBUSY;
		ret = rproc_shutdown(state->rproc);
		if (ret)
			return ret;
		state->scp_owned = false;
	}
	if (!rproc_is(state, 0, RPROC_OFFLINE))
		return -EBUSY;
	for (i = 0; i < 2; i++) {
		if (state->ipi_registered[i]) {
			scp_ipi_unregister(state->scp_api, state->channel[i].id);
			state->ipi_registered[i] = false;
		}
	}
	ret = cam_put_and_gate(state);
	if (ret)
		return ret;
	state->stop_verified = true;
	/* STREAMOFF stops the hardware. Session retirement and device remove
	 * own allocation release after joined workers and the same OFF proof. */
	return 0;
}

/* Snapshot shared CQ evidence only; no image data is exported. */
static int hw_snapshot_locked(struct integrated_state *state)
{
	struct mt8183_p1_hw_snapshot snapshot;
	u32 *words = state->composer.cpu;
	int ret;

	if (!state->resources.pm_ref || !state->resources.clocks_on || !words)
		return -EPERM;
	ret = mt8183_p1_hw_snapshot(&state->hw, &snapshot);
	if (ret)
		return ret;
	dma_rmb();
	state->cq_descriptor = READ_ONCE(words[0x268 / 4]);
	state->cq_source = READ_ONCE(words[0x26c / 4]);
	state->cq_shared_sequence = READ_ONCE(words[(0x80000 + 0x13b8) / 4]);
	state->cq_shared_base = READ_ONCE(words[(0x80000 + 0x198) / 4]);
	return 0;
}

static int stream_configure(struct integrated_state *state)
{
	struct mtk_isp_scp_p1_cmd cmd;
	int ret;

	if (state->stage != 1 || state->transport_failed)
		return -EPERM;
	ret = mt8183_p1_stream_encode_meta(&cmd, sizeof(cmd), &state->hw.profile);
	if (!ret) ret = send_cmd(state, &cmd);
	if (!ret) ret = mt8183_p1_stream_encode_config(&cmd, sizeof(cmd), &state->hw.profile);
	if (!ret) ret = send_cmd(state, &cmd);
	if (!ret) ret = hw_snapshot_locked(state);
	if (!ret) {
		struct mt8183_p1_hw_snapshot h;
		mt8183_p1_hw_observe(&state->hw, &h);
		state->pre_frame_sequence = h.frame_sequence;
		state->stage = 4;
	} else {
		state->transport_failed = true;
		mt8183_p1_hw_hold(&state->hw, ret);
	}
	return ret;
}

static int stream_arm(struct integrated_state *state)
{
	struct mtk_isp_scp_p1_cmd cmd;
	int ret;

	if (state->stage != 4 || state->transport_failed || READ_ONCE(state->hw.error) ||
	    !state->resources.pm_ref || !state->resources.clocks_on)
		return -EPERM;
	ret = mt8183_p1_stream_encode_stream(&cmd, sizeof(cmd), 1);
	if (ret) return ret;
	atomic_set(&state->irq_enabled, 1);
	enable_irq(state->resources.irq);
	ret = send_cmd(state, &cmd);
	if (!ret) state->stage = 5;
	return ret;
}

static int stream_frame(struct integrated_state *state)
{
	struct mt8183_p1_hw_snapshot snapshot;
	int ret;

	if ((state->stage != 5 && state->stage != 6) || state->transport_failed ||
	    READ_ONCE(state->hw.error) || !rproc_is(state, 1, RPROC_RUNNING))
		return -EPERM;
	state->commands++;
	ret = mt8183_p1_hw_frame_submit(&state->hw, state->scp_api);
	if (!ret) ret = hw_snapshot_locked(state);
	mt8183_p1_hw_observe(&state->hw, &snapshot);
	/* All three jobs are composed while both inputs are off. Only the first
	 * FRAME launches the initial CQ. Later FRAME replies only prove compose;
	 * their actual CAM transfer will be observed at SOF after input starts. */
	if (!ret && (snapshot.error || !snapshot.registers_valid ||
	    snapshot.sof_count || snapshot.done_count || snapshot.cq_writes ||
	    snapshot.frame_pending || !snapshot.frame_acked ||
	    snapshot.submitted_sequence < 1 || snapshot.submitted_sequence > 3 ||
	    snapshot.ack_sequence != snapshot.submitted_sequence ||
	    snapshot.ack_count != snapshot.submitted_sequence))
		ret = -EIO;
	if (!ret && snapshot.submitted_sequence == 1 && (
	    snapshot.frame_sequence != 1 || state->pre_frame_sequence != 0 || (snapshot.cq_start & 1) ||
	    snapshot.cq_base != state->cq_shared_base ||
	    state->cq_shared_base != state->composer.cam_iova + 0x12c0 ||
	    state->cq_descriptor != 0x73b8 ||
	    state->cq_source != state->composer.cam_iova + 0x813b8 ||
	    state->cq_shared_sequence != 1))
		ret = -EIO;
	if (ret) {
		state->transport_failed = true;
		mt8183_p1_hw_hold(&state->hw, ret);
	} else state->stage = 6;
	return ret;
}

static int stream_disarm(struct integrated_state *state)
{
	struct mtk_isp_scp_p1_cmd cmd;
	int ret;

	if ((state->stage != 5 && state->stage != 6) || state->transport_failed)
		return -EPERM;
	ret = mt8183_p1_stream_encode_stream(&cmd, sizeof(cmd), 0);
	if (!ret) ret = send_cmd(state, &cmd);
	if (!ret) state->stage = 7;
	return ret;
}

/* One sysfs transaction keeps userspace scheduling out of DONE -> input stop.
 * Firmware's STREAM0/DEINIT replies do not authorize release. Even a failed
 * capture always reaches the independent SCP/CAM gate and retains its memory.
 */
static int capture_conditions(struct integrated_state *state)
{
 struct v4l2_ctrl_handler *h=state->graph.sensor->ctrl_handler;
 struct v4l2_ctrl *vblank,*exposure,*gain,*pattern;
 if(!h||state->sensor_on||state->seninf_on)return -EPERM;
 vblank=v4l2_ctrl_find(h,V4L2_CID_VBLANK);exposure=v4l2_ctrl_find(h,V4L2_CID_EXPOSURE);
 gain=v4l2_ctrl_find(h,V4L2_CID_ANALOGUE_GAIN);pattern=v4l2_ctrl_find(h,V4L2_CID_TEST_PATTERN);
 if(!vblank||!exposure||!gain||!pattern)return -ENOENT;
 /* The fixed geometry has a sensor mode minimum. Standard controls own
  * timing; this check never changes libcamera exposure or blanking. */
 if(v4l2_ctrl_g_ctrl(vblank)<vblank->minimum||
    v4l2_ctrl_g_ctrl(vblank)>vblank->maximum||
    v4l2_ctrl_g_ctrl(pattern)!=0)return -EINVAL;
 state->requested_exposure=v4l2_ctrl_g_ctrl(exposure);
 state->requested_gain=v4l2_ctrl_g_ctrl(gain);
 dev_info(state->resources.dev,"P1_SENSOR_CACHE vblank=%d exposure_code=%u gain_code=%u pattern=0 applied_frame=unknown\n",v4l2_ctrl_g_ctrl(vblank),state->requested_exposure,state->requested_gain);
 return 0;
}

/* VB2 stream workers. Failed stop retains independent hardware allocations. */
static void worker_close(void *context)
{
    struct integrated_state *state=context;
    mt8183_p1_hw_stopping(&state->hw);
    dph_close(&state->handoff);
    /* Command waits remain bounded and validate their real reply count;
     * waking does not fabricate firmware acknowledgement. */
    complete_all(&state->channel[0].reply);
}
static int start_open(struct integrated_state *state,int result)
{
    if(result)return result;
    return dpw_stopping(&state->workers)?-ECANCELED:READ_ONCE(state->hw.error);
}
static int worker_start(void *context)
{
    struct integrated_state *state=context;
    unsigned int frame;int ret;
    ret=start_open(state,capture_conditions(state));
    if(!ret)ret=start_open(state,stream_configure(state));
    if(!ret)ret=start_open(state,stream_arm(state));
    for(frame=0;frame<3&&!ret;frame++)ret=start_open(state,stream_frame(state));
    if(!ret){mt8183_p1_hw_irq(&state->hw);ret=start_open(state,0);}
    if(!ret)ret=start_open(state,mt8183_p1_hw_inputs_begin(&state->hw));
    if(!ret){
        state->inputs_idle=false;
        ret=v4l2_subdev_call(state->graph.seninf,video,s_stream,1);
        if(!ret)state->seninf_on=true;
        ret=start_open(state,ret);
    }
    if(!ret){
        ret=v4l2_subdev_call(state->graph.sensor,video,s_stream,1);
        if(!ret)state->sensor_on=true;
        ret=start_open(state,ret);
    }
    /* Successful commit is the last operation that may fail. Coordinator
     * waits start_done before consuming the input-start references above. */
    if(!ret)ret=mt8183_p1_hw_start_commit(&state->hw);
    if(ret)mt8183_p1_hw_hold(&state->hw,ret);
    return ret;
}
static int worker_publish_request(void *context,u64 logical)
{
    struct integrated_state *state=context;
    return dph_send(&state->handoff,logical);
}
static int worker_capture(void *context)
{
    struct integrated_state *state=context;
    struct mt8183_p1_hw_snapshot h;int ret;
    ret=mt8183_p1_hw_refill_remaining(&state->hw,state->scp_api,worker_publish_request,state);
    mt8183_p1_hw_observe(&state->hw,&h);
    state->commands+=h.live_send_attempts;
    if(ret)state->transport_failed=true;
    /* Refill runs until explicit phase-boundary stop; no finite DONE wait. */
    if(ret)mt8183_p1_hw_hold(&state->hw,ret);
    return ret;
}
static int worker_publish_one(void *context,u64 logical)
{
    struct integrated_state *state=context;
    return mt8183_p1_hw_publish_image(&state->hw,logical);
}
static int worker_publish(void *context)
{
    struct integrated_state *state=context;
    return dph_run(&state->handoff,worker_publish_one,state);
}
static int worker_input_off(void *context)
{
    struct integrated_state *state=context;
    int ret=capture_inputs_stop(state);
    if(!ret)ret=mt8183_p1_hw_inputs_stopped(&state->hw,state->inputs_idle);
    return ret?:state->input_error;
}
static int worker_return_cpu(void *context,bool committed)
{
    struct integrated_state *state=context;
    /* dps chooses QUEUED/ERROR from the same completed start transaction. */
    if(committed!=state->hw.stream.start_committed)return -EPROTO;
    return mt8183_p1_hw_return_cpu(&state->hw);
}
static int worker_gate(void *context)
{
    struct integrated_state *state=context;
    struct mtk_isp_scp_p1_cmd cmd;
    int ret,stop_ret;
    /* input_off and real joins precede this callback. No frame sender or
     * publisher remains while command teardown and the raw OFF gate run. */
    ret=READ_ONCE(state->hw.error)?:READ_ONCE(state->workers.first_error);
    if(!ret)ret=state->workers.input_error?:state->workers.cpu_error;
    if(!ret){struct mt8183_p1_hw_snapshot last;ret=mt8183_p1_hw_snapshot(&state->hw,&last);}
    if(!ret)ret=stream_disarm(state);
    if(!ret){
        ret=mt8183_p1_encode_deinit(&cmd);
        if(!ret)ret=send_cmd(state,&cmd);
        if(!ret)state->stage=2;
    }
    if(ret)mt8183_p1_hw_hold(&state->hw,ret);
    stop_ret=stream_stop(state);state->stop_error=stop_ret;
    if(!ret)ret=stop_ret;
    if(!ret&&!state->stop_verified)ret=-EIO;
    if(!ret)ret=mt8183_p1_hw_ring_finalize(&state->hw,state->stop_verified);
    /* stop_verified alone does not describe capture/publication success. */
    return ret;
}
static const struct dpw_ops camera_worker_ops={
    .close=worker_close,.start=worker_start,.capture=worker_capture,
    .publish=worker_publish,.input_off=worker_input_off,
    .return_cpu=worker_return_cpu,.gate=worker_gate,
};
static void video_frame_start(void *context,u64 logical)
{
 struct integrated_state *s=context;
 struct v4l2_event event={.type=V4L2_EVENT_FRAME_SYNC};
 /* Same logical sequence as the VB2 buffer, including dropped frames. */
 event.u.frame_sync.frame_sequence=(u32)logical;
 v4l2_subdev_notify_event(&s->graph.subdev,&event);
}
static int video_deliver(void *context,const u8 *raw,size_t bytes,u64 logical,u64 timestamp)
{return dcv_deliver(&((struct integrated_state *)context)->video,raw,bytes,logical,timestamp);}
/* Serial endpoint restart after real worker joins, child idle, SCP OFF, IPI
 * unregister and fresh CAM/protection proof. Graph/SCP provider lifetime stays
 * resident. New epoch gets newly allocated DMA/CPU objects and fresh workers. */
static int video_retire(struct integrated_state *s)
{
 s64 next;unsigned int i;int ret;
 if(!s->capture_attempted)return 0;
 if(!s->workers.drained||s->workers.state!=DPW_STOPPED||s->workers_active||
    s->workers.capture_task||s->workers.publish_task||s->workers.coordinator||
    s->handoff.active||s->handoff.pending||!s->stop_verified||!s->hw.finalized||
    s->hw.error||s->transport_failed||!s->inputs_idle||s->sensor_on||s->seninf_on||
    s->scp_owned||s->ipi_registered[0]||s->ipi_registered[1]||atomic_read(&s->irq_enabled)||
    s->resources.pm_ref||s->resources.clocks_on||!rproc_is(s,0,RPROC_OFFLINE))return -EBUSY;
 ret=capture_child_idle(s->graph.sensor->dev);if(!ret)ret=capture_child_idle(s->graph.seninf->dev);
 if(!ret)ret=cam_put_and_gate(s);if(ret)return ret;
 synchronize_irq(s->resources.irq);
 next=atomic64_inc_return(&session_epochs);if(next<=0)return -EOVERFLOW;
 ret=mt8183_p1_hw_release(&s->hw,true);if(ret)return ret;
 if(s->graph.sensor_pinned){module_put(s->graph.sensor->owner);put_device(s->graph.sensor->dev);s->graph.sensor_pinned=false;}
 if(s->graph.seninf_pinned){module_put(s->graph.seninf->owner);put_device(s->graph.seninf->dev);s->graph.seninf_pinned=false;}
 s->graph.sensor=NULL;s->graph.prepared=false;
 s->retired_epoch=s->session_epoch;s->retired_sessions++;s->retired_cpu_allocations+=s->hw.observed.copy_allocations;s->retired_cpu_frees+=s->hw.observed.copy_frees;
 memset(&s->hw,0,sizeof(s->hw));memset(&s->workers,0,sizeof(s->workers));memset(&s->handoff,0,sizeof(s->handoff));
 memset(s->composer.cpu,0,TEST_SIZE);dma_wmb();
 for(i=0;i<2;i++){s->channel[i].count=s->channel[i].len=0;memset(s->channel[i].bytes,0,sizeof(s->channel[i].bytes));reinit_completion(&s->channel[i].reply);}
 s->session_epoch=(u64)next;s->stage=0;s->commands=0;s->fw_exposed=false;s->cq_descriptor=s->cq_source=s->cq_shared_sequence=s->cq_shared_base=s->pre_frame_sequence=0;
 s->control_error=s->send_error=s->stop_error=s->capture_error=0;s->capture_attempted=false;s->capture_ok=false;s->retain_capture=false;s->stop_verified=false;s->stopping=false;
 atomic_set(&s->session_irq_calls,0);memset(&s->stop_snapshot,0,sizeof(s->stop_snapshot));
 s->video_needs_prepare=true;
 return 0;
}
/* 2026-10-06: the graph is idle before its first STREAMON too. Drop the
 * unpublished DMA before system sleep, after normal driver binding.
 * No STOP proof is invented: this path only frees objects never given to SCP.
 * A later STREAMON allocates a fresh profile and performs the real OFF cycle.
 */
static int video_initial_idle(struct integrated_state *s)
{
 int ret;
 if(s->capture_attempted||s->hw.published||s->fw_exposed||s->workers_active||
    s->scp_owned||s->sensor_on||s->seninf_on||s->ipi_registered[0]||
    s->ipi_registered[1]||atomic_read(&s->irq_enabled)||s->transport_failed)
  return -EBUSY;
 ret=cam_put_and_gate(s);if(ret)return ret;
 ret=mt8183_p1_hw_release(&s->hw,false);if(ret)return ret;
 s->retired_cpu_allocations+=s->hw.observed.copy_allocations;
 s->retired_cpu_frees+=s->hw.observed.copy_frees;
 memset(&s->hw,0,sizeof(s->hw));
 s->inputs_idle=true;s->video_needs_prepare=true;s->cycle_verified=false;
 return 0;
}
static bool video_unpublished_idle(struct integrated_state *s)
{
 return !s->capture_attempted&&!s->hw.published&&!s->hw.error&&
        !s->fw_exposed&&s->inputs_idle&&
        !s->workers_active&&!s->stream_module_ref&&!s->scp_owned&&
        !s->sensor_on&&!s->seninf_on&&!s->ipi_registered[0]&&
        !s->ipi_registered[1]&&!atomic_read(&s->irq_enabled)&&
        !s->resources.pm_ref&&!s->resources.clocks_on&&
        !s->capture_error&&!s->input_error&&!s->transport_failed;
}
static int video_rearm(struct integrated_state *s)
{
 int ret=video_retire(s);
 if(ret)return ret;
 if(!s->video_needs_prepare) {
  if(!s->resources.pm_ref) {
   ret=pm_runtime_resume_and_get(s->resources.dev);if(ret<0)return ret;
   s->resources.pm_ref=true;COUNT(pm_gets);
  }
  return 0;
 }
 ret=pm_runtime_resume_and_get(s->resources.dev);if(ret<0)return ret;s->resources.pm_ref=true;COUNT(pm_gets);
 ret=mt8183_p1_hw_prepare_profile(&s->hw,s->resources.dev,s->resources.base,s->composer.cam_iova,s->session_epoch,s->video.width,s->video.height);
 if(ret){s->transport_failed=true;return ret;}s->hw.profile.bayer_id=dcv_bayer_index(s->sensor_code);s->video_needs_prepare=false;s->rearms++;return 0;
}
static int video_start(void *context)
{
 struct integrated_state *s=context;int ret;mutex_lock(&s->control_lock);
 if(s->system_sleep_pending){mutex_unlock(&s->control_lock);return -EBUSY;}
 /* Keep callback code resident whenever DMA may have been exposed. A failed
  * start/stop holds this reference; ordinary successful stop releases it. */
 if(s->stream_module_ref){mutex_unlock(&s->control_lock);return -EBUSY;}
 if(!try_module_get(THIS_MODULE)){mutex_unlock(&s->control_lock);return -ENODEV;}
 s->stream_module_ref=true;
 ret=video_rearm(s);
 if(!ret&&!s->capture_attempted&&(s->hw.profile.width!=s->video.width||s->hw.profile.height!=s->video.height)){
  ret=mt8183_p1_hw_release(&s->hw,false);
  if(!ret){memset(&s->hw,0,sizeof(s->hw));ret=mt8183_p1_hw_prepare_profile(&s->hw,s->resources.dev,s->resources.base,s->composer.cam_iova,s->session_epoch,s->video.width,s->video.height);}
 }
 if(!ret&&!s->cycle_verified)ret=stream_cycle(s);
 if(!ret&&!s->graph.prepared)ret=capture_prepare(s);
 if(!ret){
  struct v4l2_subdev_format fmt={.which=V4L2_SUBDEV_FORMAT_ACTIVE,.pad=0};
  ret=v4l2_subdev_call(s->graph.sensor,pad,get_fmt,NULL,&fmt);
  if(!ret){int index=dcv_bayer_index(fmt.format.code);
   if(index<0)ret=index;
   else {s->sensor_code=fmt.format.code;s->hw.profile.bayer_id=index;}
  }
 }
 if(!ret)ret=stream_boot_init(s);
 if(ret)goto failed;
 s->hw.deliver=video_deliver;s->hw.deliver_context=s;
 s->hw.frame_start=video_frame_start;s->hw.frame_start_context=s;
 s->capture_attempted=true;s->retain_capture=true;dph_init(&s->handoff);
 /* Frame-deadline workers use the same lowest FIFO priority as threaded
  * IRQ work. No CPU number, affinity or frequency policy is imposed.
  * They sleep on frame events and retain the bounded real-join protocol. */
 ret=dpw_init(&s->workers,&camera_worker_ops,s);
 if(ret)goto failed;
 sched_set_fifo_low(s->workers.capture_task);
 sched_set_fifo_low(s->workers.publish_task);
 smp_store_release(&s->workers_active,true);ret=dpw_start(&s->workers);
 if(ret){wait_for_completion(&s->workers.stop_done);dpw_drain(&s->workers);smp_store_release(&s->workers_active,false);goto failed;}
 mutex_unlock(&s->control_lock);return 0;
failed:
 /* Before stream publication or after a joined failed start, no VB2 payload
  * lease remains. HW/FW state is retained on error; reopen becomes blocked. */
 s->capture_error=ret;s->transport_failed=true;mutex_unlock(&s->control_lock);return ret;
}
static int video_stop(void *context)
{
 struct integrated_state *s=context;int ret=0;mutex_lock(&s->control_lock);
 if(s->workers.initialized&&!s->workers.drained){
  mt8183_p1_hw_request_stop(&s->hw);wait_for_completion(&s->workers.stop_done);
  ret=dpw_drain(&s->workers);smp_store_release(&s->workers_active,false);
 }
 if(!ret&&(!s->workers.drained||!s->stop_verified||!s->hw.finalized||s->hw.error||!s->inputs_idle||s->scp_owned||s->ipi_registered[0]||s->ipi_registered[1]||atomic_read(&s->irq_enabled)))ret=-EBUSY;
 s->capture_error=ret;s->capture_ok=!ret;
 dev_info(s->resources.dev,"MT8183_P1_STREAM_STOP ret=%d epoch=%llu sof=%u done=%u delivered=%llu dropped=%llu joined=%u off=%u\n",ret,(unsigned long long)s->session_epoch,s->hw.observed.sof_count,s->hw.observed.done_count,(unsigned long long)s->video.delivered,(unsigned long long)s->video.dropped,s->workers.drained,s->stop_verified);
 if(!ret&&s->stream_module_ref){s->stream_module_ref=false;module_put(THIS_MODULE);}
 mutex_unlock(&s->control_lock);return ret;
}

static ssize_t result_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct integrated_state *state = dev_get_drvdata(dev);
	struct resource_state *s = &state->resources;
	struct composer_state *c = &state->composer;
	unsigned long flags;
	u32 rx0, rx1, len0, len1;
	u8 bytes0[16], bytes1[16];
	int ret;

	mutex_lock(&state->control_lock);
	spin_lock_irqsave(&state->rx_lock, flags);
	rx0 = state->channel[0].count; rx1 = state->channel[1].count;
	len0 = state->channel[0].len; len1 = state->channel[1].len;
	memcpy(bytes0, state->channel[0].bytes, 16);
	memcpy(bytes1, state->channel[1].bytes, 16);
	spin_unlock_irqrestore(&state->rx_lock, flags);
	ret = scnprintf(buf, PAGE_SIZE,
		"stage=%u stopped=%u fw_exposed=%u cycle_verified=%u cycles=%u stop_verified=%u control_error=%d send_error=%d stop_error=%d transport_failed=%u scp_owned=%u ipi10=%u ipi11=%u commands=%u rx10=%u len10=%u bytes10=%*phN rx11=%u len11=%u bytes11=%*phN allocated=%u mapped=%u pages=%u scp_dma=%pad cam_iova=%pad pm_ref=%u pm_usage=%d clocks_on=%u irq_owned=%u irq_calls=%d\n",
		state->stage, state->stopped, state->fw_exposed, state->cycle_verified,
		state->cycles, state->stop_verified, state->control_error, state->send_error,
		state->stop_error, state->transport_failed, state->scp_owned,
		state->ipi_registered[0], state->ipi_registered[1], state->commands,
		rx0, len0, 16, bytes0, rx1, len1, 16, bytes1,
		!!c->cpu, c->mapped, c->mapped_pages, &c->scp_dma, &c->cam_iova,
		s->pm_ref, atomic_read(&dev->power.usage_count), s->clocks_on,
		s->irq_owned, atomic_read(&irq_calls));
	mutex_unlock(&state->control_lock);
	return ret;
}
static ssize_t stop_snapshot_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct integrated_state *state = dev_get_drvdata(dev);
	struct mt8183_p1_stop_snapshot *s = &state->stop_snapshot;
	int ret;

	mutex_lock(&state->control_lock);
	ret = scnprintf(buf, PAGE_SIZE, "pwr_status=%08x pwr_status_2nd=%08x cam_ctl=%08x infra_mm_sta=%08x infra_sta=%08x smi_sta=%08x infra_mm_en=%08x valid_mask=%x samples=%u elapsed_us=%llu event_seq=%llu arm_seq=%llu off_seq=%llu armed=%u pending_off=%u provider_off=%u notifier_registered=%u read_error=%d passed=%u\n",
		s->pwr_status, s->pwr_status_2nd, s->cam_ctl, s->infra_mm_sta,
		s->infra_sta, s->smi_sta, s->infra_mm_en, s->valid_mask, s->samples,
		(unsigned long long)s->elapsed_us, (unsigned long long)s->event_seq,
		(unsigned long long)s->arm_seq, (unsigned long long)s->off_seq,
		s->armed, s->pending_off, s->provider_off,
		state->stop_gate.notifier_registered, s->read_error, s->passed);
	mutex_unlock(&state->control_lock);
	return ret;
}
static ssize_t hw_result_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct integrated_state *state = dev_get_drvdata(dev);
	struct mt8183_p1_hw_snapshot h;
	int ret;

	mutex_lock(&state->control_lock);
	mt8183_p1_hw_observe(&state->hw, &h);
	ret = scnprintf(buf, PAGE_SIZE, "allocated=%u published=%u frame_pending=%u frame_acked=%u output_iova=%pad image_bytes=%u allocation_bytes=%u error=%d send_error=%d submitted=%u ack_count=%u ack_channel=%u ack_sequence=%u",
		h.allocated, h.published, h.frame_pending, h.frame_acked, &h.output_iova,
		h.image_bytes, h.allocation_bytes, h.error, h.send_error, h.submitted_sequence,
		h.ack_count, h.ack_channel, h.ack_sequence);
	/* Keep the snapshot plus ARM64 variadic arguments below the stack budget. */
	ret += scnprintf(buf + ret, PAGE_SIZE - ret, " irq_enabled=%d irq_count=%u irq_status=%08x irq_status2=%08x irq_sequence=%u cq_writes=%u cq_arms_accepted=%u done_count=%u registers_valid=%u cq_start=%08x cq_base=%08x frame_sequence=%u descriptor=%08x cq_source=%08x shared_sequence=%u cq_shared_base=%08x pre_frame_sequence=%u\n",
		atomic_read(&state->irq_enabled),
		h.irq_count, h.irq_status, h.irq_status2, h.irq_sequence, h.cq_writes, h.cq_arms_accepted, h.done_count,
		h.registers_valid, h.cq_start, h.cq_base, h.frame_sequence,
		state->cq_descriptor, state->cq_source, state->cq_shared_sequence,
		state->cq_shared_base, state->pre_frame_sequence);
	mutex_unlock(&state->control_lock);
	return ret;
}

static ssize_t capture_result_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct integrated_state *s=dev_get_drvdata(dev);
    struct mt8183_p1_hw_snapshot h;
    ssize_t len;
    mutex_lock(&s->control_lock);
    mt8183_p1_hw_observe(&s->hw,&h);
    len=scnprintf(buf, PAGE_SIZE,"schema=3 frame_limit=0 spans=6 prepared=%u capture_ok=%u capture_error=%d input_error=%d inputs_idle=%u retain_capture=%u unpublished_idle=%u\n",
        s->graph.prepared,s->capture_ok,s->capture_error,s->input_error,s->inputs_idle,s->retain_capture,video_unpublished_idle(s));
    len+=scnprintf(buf + len, PAGE_SIZE - len,"epoch=%llu sof_count=%u done_count=%u done_sequence=%u archived=%u finalized=%u pending=%u refs=%u raw_bytes=%u copy_allocations=%u copy_frees=%u\n",
        (unsigned long long)h.epoch,h.sof_count,h.done_count,h.done_sequence,h.archived_frames,h.ring_finalized,
        h.ring_pending_sequence,h.ring_cpu_refs,h.logical_raw_bytes,h.copy_allocations,h.copy_frees);
    mutex_unlock(&s->control_lock);return len;
}
static ssize_t session_result_show(struct device *dev,struct device_attribute *attr,char *buf)
{
    struct integrated_state *s=dev_get_drvdata(dev);ssize_t n;
    mutex_lock(&s->control_lock);
    n=scnprintf(buf, PAGE_SIZE,"schema=1 epoch=%llu retired_epoch=%llu rearms=%u retired_sessions=%u retired_cpu_allocations=%u retired_cpu_frees=%u rearm_error=%d graph_registered=%u session_irq_calls=%d\n",
      (unsigned long long)s->session_epoch,(unsigned long long)s->retired_epoch,s->rearms,
      s->retired_sessions,s->retired_cpu_allocations,s->retired_cpu_frees,s->rearm_error,s->graph.media_registered,atomic_read(&s->session_irq_calls));
    mutex_unlock(&s->control_lock);return n;
}

/* Diagnostics are optional, root-only snapshots. They neither start a
 * stream nor expose image data, and are not an application ABI. The short
 * debugfs proxy pins each read until removal has drained it; no seq_file
 * state or open descriptor retains a pointer after driver teardown. */
struct p1_diagnostic {
	const char *name;
	ssize_t (*show)(struct device *, struct device_attribute *, char *);
};

static ssize_t stats_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	return get_stats(buf, NULL);
}

static const struct p1_diagnostic p1_diagnostics[] = {
	{ "result", result_show },
	{ "stop_snapshot", stop_snapshot_show },
	{ "hw_result", hw_result_show },
	{ "capture_result", capture_result_show },
	{ "session_result", session_result_show },
	{ "stats", stats_show },
};

static ssize_t p1_debug_read(struct file *file, char __user *user,
			     size_t count, loff_t *position)
{
	const struct p1_diagnostic *diagnostic = debugfs_get_aux(file);
	char *buf;
	ssize_t len, ret;

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	len = diagnostic->show(file->private_data, NULL, buf);
	ret = len < 0 ? len : simple_read_from_buffer(user, count, position, buf, len);
	kfree(buf);
	return ret;
}

static const struct debugfs_short_fops p1_debug_fops = {
	.read = p1_debug_read,
	.llseek = default_llseek,
};

static void p1_debug_init(struct integrated_state *state)
{
	unsigned int i;

	if (IS_ERR_OR_NULL(p1_debug_root))
		return;
	state->debug_dir = debugfs_create_dir(dev_name(state->resources.dev), p1_debug_root);
	if (IS_ERR_OR_NULL(state->debug_dir))
		return;
	for (i = 0; i < ARRAY_SIZE(p1_diagnostics); i++)
		debugfs_create_file_aux(p1_diagnostics[i].name, 0400, state->debug_dir,
			state->resources.dev, &p1_diagnostics[i], &p1_debug_fops);
}

static int integrated_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *scp_node;
	struct platform_device *scp_pdev;
	struct integrated_state *state;
	struct resource *res;
	struct reserved_mem *pool;
	struct device_link *link;
	bool supplier_locked = false;
	int ret, cleanup;

	COUNT(probes);
	ret = validate_node(dev->of_node);
	if (ret || dev->of_node != test_node || !dev->pm_domain) {
		ret = ret ?: -EINVAL;
		goto early_fail;
	}
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start != CAM_BASE || resource_size(res) != CAM_SIZE ||
	    platform_get_resource(pdev, IORESOURCE_MEM, 1) || res->flags != IORESOURCE_MEM) {
		ret = -EINVAL;
		goto early_fail;
	}
	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state) { ret = -ENOMEM; goto early_fail; }
	state->resources.dev = dev;
	state->resources.clocks[0].id = "cam";
	state->resources.clocks[1].id = "camtg";
	state->composer.cam = dev;
	state->graph.dev = dev;
	mutex_init(&state->control_lock);
	spin_lock_init(&state->rx_lock);
	atomic_set(&state->irq_enabled, 0);
	atomic_set(&state->session_irq_calls, 0);
	state->channel[0].state = state;
	state->channel[0].id = SCP_IPI_ISP_CMD;
	state->channel[1].state = state;
	state->channel[1].id = SCP_IPI_ISP_FRAME;
	init_completion(&state->channel[0].reply);
	init_completion(&state->channel[1].reply);
	platform_set_drvdata(pdev, state);
	if (of_count_phandle_with_args(dev->of_node, "mediatek,scp", NULL) != 1) {
		ret = -EINVAL;
		goto fail;
	}
	scp_node = of_parse_phandle(dev->of_node, "mediatek,scp", 0);
	if (!scp_node) {
		ret = -EINVAL;
		goto fail;
	}
	scp_pdev = of_find_device_by_node(scp_node);
	of_node_put(scp_node);
	if (!scp_pdev) {
		ret = -ENODEV;
		goto fail;
	}
	state->composer.scp = &scp_pdev->dev; /* Own the of_find_device_by_node reference. */
	if (!device_trylock(state->composer.scp)) {
		ret = -EBUSY;
		goto fail;
	}
	supplier_locked = true;
	/* A driver still on knode may already be unbinding while its lock is
	 * dropped to detach consumers. Do not attach in that window.
	 */
	if (READ_ONCE(state->composer.scp->links.status) != DL_DEV_DRIVER_BOUND) {
		ret = -ENODEV;
		goto fail;
	}
	ret = validate_scp_pool(state->composer.scp, &pool);
	if (ret)
		goto fail;
	link = device_link_add(dev, state->composer.scp, DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		ret = -EINVAL;
		goto fail;
	}
	/* Managed link: driver core owns deletion, including failed probe. It
	 * orders our remove before SCP pool release. Never use device_link_del.
	 * The supplier lock is held only for this finite, no-IPI probe.
	 */
	ret = composer_acquire(&state->composer, pool);
	if (ret)
		goto fail;
	device_unlock(state->composer.scp);
	supplier_locked = false;
	ret = resources_acquire(pdev, &state->resources);
	if (ret)
		goto fail;
	/* Never reset or wrap the identity of a live/failed session. */
	{
		s64 epoch = atomic64_inc_return(&session_epochs);
		if (epoch <= 0) { ret = -EOVERFLOW; goto fail; }
		state->session_epoch = (u64)epoch;
	}
	ret = mt8183_p1_hw_prepare(&state->hw, dev, state->resources.base,
				state->composer.cam_iova, state->session_epoch);
	if (ret)
		goto fail;
	ret = graph_register(&state->graph);
	if (ret)
		goto fail;
	/* Hold only SMI common (DISP domain), never a CAM larb reference. */
	{
		struct device_node *node = of_find_node_by_path("/soc/smi@14019000");
		struct platform_device *common = node ? of_find_device_by_node(node) : NULL;

		of_node_put(node);
		if (!common) { ret = -ENODEV; goto fail; }
		state->smi_dev = &common->dev;
		if (!device_is_bound(state->smi_dev) ||
		    READ_ONCE(state->smi_dev->links.status) != DL_DEV_DRIVER_BOUND) {
			ret = -ENODEV; goto fail;
		}
		if (!device_link_add(dev, state->smi_dev, DL_FLAG_AUTOREMOVE_CONSUMER)) {
			ret = -EINVAL; goto fail;
		}
		ret = pm_runtime_resume_and_get(state->smi_dev);
		if (ret < 0) goto fail;
		state->smi_ref = true;
	}
	state->scp_api = scp_get(pdev);
	if (!state->scp_api) { ret = -ENODEV; goto fail; }
	state->rproc = scp_get_rproc(state->scp_api);
	if (!state->rproc) { ret = -ENODEV; goto fail; }
	/* All published nodes pin the parent until their core release completes. */
	ret = mt8183_p1_stop_gate_init(&state->stop_gate, dev);
	if (ret)
		goto fail;
	ret=dcv_register(&state->video,dev,&state->graph.v4l2,state,video_start,video_stop,
		&state->graph.subdev.entity,P1_SOURCE_PAD);
	if(ret)goto fail;
	ret=v4l2_device_register_subdev_nodes(&state->graph.v4l2);
	if(ret)goto fail;
	ret=media_device_register(&state->graph.media);
	if(ret)goto fail;
	state->graph.media_registered=true;COUNT(media_regs);
	state->system_notifier.notifier_call = resources_system_notify;
	ret = register_pm_notifier(&state->system_notifier);
	if (ret) goto fail;
	state->system_notifier_registered = true;
	/* Drop the probe reference asynchronously. A device still being probed
	 * cannot complete genpd OFF, so never wait for the raw OFF gate here.
	 * The first STREAMON takes a fresh reference and proves its normal real
	 * OFF cycle; pre-device system PM retires unpublished DMA if still idle.
	 */
	ret = mt8183_p1_stop_gate_arm(&state->stop_gate);
	if (ret) goto fail;
	state->inputs_idle = true;
	ret = pm_runtime_put(dev);
	state->resources.pm_ref = false;COUNT(pm_puts);
	if (ret < 0) goto fail;
	/* Last fallible publication completed; media/video/subdev graph ready. */
	smp_store_release(&state->video.registered,true);
	finish_stats(0, true);
	p1_debug_init(state);
	dev_info(dev, "P1_STREAM_HELD bytes=%lu pages=%u irq=%d no_fw=1 no_stream=1\n",
		(unsigned long)TEST_SIZE, state->composer.mapped_pages, state->resources.irq);
	return 0;
fail:
	if (state->system_notifier_registered) {
		unregister_pm_notifier(&state->system_notifier);
		state->system_notifier_registered = false;
	}
	if (supplier_locked) {
		struct device *scp = get_device(state->composer.scp);

		cleanup = integrated_release(state);
		device_unlock(scp);
		put_device(scp);
	} else {
		cleanup = integrated_release(state);
	}
	platform_set_drvdata(pdev, NULL);
	kfree(state);
	finish_stats(ret, false);
	dev_info(dev, "P1_INTEGRATED_UNWOUND error=%d cleanup_error=%d\n", ret, cleanup);
	return ret;
early_fail:
	finish_stats(ret, false);
	return ret;
}

/* No live video/subdev FD or failed stream permits ordinary module unload.
 * A failed hardware stop holds stream_module_ref and its independent memory.
 * Provider-forced teardown remains outside this non-hotpluggable candidate. */
static void integrated_remove(struct platform_device *pdev)
{
 struct integrated_state *state=platform_get_drvdata(pdev);
 int ret;
 if(!state)return;
 /* Drain diagnostic readers before taking their control mutex or freeing state. */
 debugfs_remove(state->debug_dir);
 state->debug_dir=NULL;
 if(state->system_notifier_registered){
  unregister_pm_notifier(&state->system_notifier);
  state->system_notifier_registered=false;
 }
 mutex_lock(&state->control_lock);
 if(state->stream_module_ref){
  dev_err(&pdev->dev,"P1_REMOVE_RETAINED active or failed stream\n");
  mutex_unlock(&state->control_lock);return;
 }
 ret=integrated_release(state);
 mutex_unlock(&state->control_lock);
 /* resources_release may report a PM error after it has drained/freed all
  * callbacks. Such a terminal cleanup error is reported, not a DMA hold. */
 if(ret && (state->stop_gate.notifier_registered || state->hw.output_cpu ||
    state->video.node_registered || state->video.queue_initialized ||
    state->graph.v4l2_registered || state->graph.notifier_registered ||
    state->resources.irq_owned || state->resources.pm_enabled ||
    state->resources.base || state->composer.cpu || state->scp_api || state->smi_dev)){
  dev_err(&pdev->dev,"P1_REMOVE_RETAINED cleanup=%d\n",ret);return;
 }
 if(ret)dev_warn(&pdev->dev,"P1_REMOVE_CLEANUP_ERROR %d after callback drainage\n",ret);
 platform_set_drvdata(pdev,NULL);
 mutex_destroy(&state->control_lock);
 kfree(state);
 dev_info(&pdev->dev,"P1_REMOVE_RELEASED\n");
}
static const struct of_device_id integrated_match[] = {
	{ .compatible = COMPAT },
	{ .compatible = MT8183_P1_LEGACY_COMPAT }, { }
};
MODULE_DEVICE_TABLE(of, integrated_match);
static struct platform_driver integrated_driver = {
	.probe = integrated_probe,
	.remove = integrated_remove,
	.driver = {
		.name = "mtk-cam-p1-raw",
		.suppress_bind_attrs = true,
		.of_match_table = integrated_match,
		.pm = &resources_pm_ops,
	},
};
static int __init integrated_init(void)
{
	int ret;

	ret = validate_tree();
	if (ret)
		return ret;
	p1_debug_root = debugfs_create_dir("mt8183_p1", NULL);
	ret = platform_driver_register(&integrated_driver);
	if (ret) {
		debugfs_remove(p1_debug_root);
		of_node_put(test_node); test_node = NULL;
	}
	return ret;
}
module_init(integrated_init);
static void __exit integrated_exit(void)
{
 platform_driver_unregister(&integrated_driver);
 debugfs_remove(p1_debug_root);
 of_node_put(test_node);
 test_node=NULL;
}
module_exit(integrated_exit);
MODULE_DESCRIPTION("MT8183 P1 standard RAW V4L2/VB2 camera with standard RAW10 and explicit phase-boundary stop");
MODULE_LICENSE("GPL");
