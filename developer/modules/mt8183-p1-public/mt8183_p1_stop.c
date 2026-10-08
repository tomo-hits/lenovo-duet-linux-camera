/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
// SPDX-License-Identifier: GPL-2.0-only
/* Read-only MT8183 CAM power-off observations for the controlled RAM harness.
 * Register contract: pinned Linux 6.18.28-mt81, drivers/pmdomain/mediatek/
 * mt8183-pm-domains.h, mtk-pm-domains.c and soc/mediatek/infracfg.h (GPL-2.0).
 * No direct MMIO mapping/writes, PM operations, or firmware operations here.
 */
#include "mt8183_p1_stop.h"

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/ioport.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/string.h>

#define SPM_PATH "/soc/syscon@10006000"
#define INFRA_PATH "/soc/syscon@10001000"
#define SMI_PATH "/soc/smi@14019000"
#define PROVIDER_PATH SPM_PATH "/power-controller"

#define CAM_POWER_BIT BIT(25)
#define DISP_POWER_BIT BIT(3)
#define CAM_CTL_MASK 0x331fU
#define CAM_CTL_OFF 0x3312U
#define INFRA_MM_MASK 0x2a30U
/* Observed on MT8183: ACK bit13 deasserts after power-off. Require its
 * completed provider handshake AND retained EN request, not a persistent ACK.
 */
#define INFRA_MM_RETAINED_ACK_MASK 0x0a30U
#define INFRA_MASK BIT(28)
#define SMI_MASK 0x18U
#define VALID_ALL 0x7fU
#define WAIT_US 8000000LL

static bool fixed_resource(struct device_node *np, const char *compatible,
			   resource_size_t start)
{
	struct resource res;

	return np && of_device_is_available(np) &&
		of_device_is_compatible(np, compatible) &&
		!of_address_to_resource(np, 0, &res) &&
		res.start == start && resource_size(&res) == 0x1000 &&
		of_address_to_resource(np, 1, &res) != 0;
}

/* Follow the same first legacy reference as the pinned provider's probe. */
static bool provider_reference(struct device_node *provider,
			       const char *property, struct device_node *expected)
{
	struct device_node *holder, *target;
	bool matches;

	holder = of_find_node_with_property(of_node_get(provider), property);
	if (!holder)
		return false;
	target = of_parse_phandle(holder, property, 0);
	matches = target == expected;
	of_node_put(target);
	of_node_put(holder);
	return matches;
}

/* GENPD_NOTIFY_OFF is also sent on failed power-on. Only a PRE_OFF -> OFF
 * pair witnesses successful completion of the pinned power-off provider.
 * No MMIO, PM calls, sleeping, or parent control lock in this raw notifier.
 */
static int power_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct mt8183_p1_stop_gate *gate =
		container_of(nb, struct mt8183_p1_stop_gate, power_nb);
	unsigned long flags;

	(void)data;
	spin_lock_irqsave(&gate->lock, flags);
	gate->event_seq++;
	switch (event) {
	case GENPD_NOTIFY_PRE_ON:
		gate->armed = false;
		gate->pending_off = false;
		gate->provider_off = false;
		break;
	case GENPD_NOTIFY_ON:
		gate->pending_off = false;
		gate->provider_off = false;
		break;
	case GENPD_NOTIFY_PRE_OFF:
		gate->pending_off = true;
		gate->provider_off = false;
		break;
	case GENPD_NOTIFY_OFF:
		if (gate->pending_off) {
			gate->off_seq++;
			gate->provider_off = gate->armed &&
				gate->off_seq > gate->arm_seq;
		}
		gate->pending_off = false;
		break;
	default:
		gate->armed = false;
		gate->pending_off = false;
		gate->provider_off = false;
		break;
	}
	spin_unlock_irqrestore(&gate->lock, flags);
	return NOTIFY_OK;
}

int mt8183_p1_stop_gate_init(struct mt8183_p1_stop_gate *gate, struct device *dev)
{
	struct device_node *provider, *spm, *infra, *smi;
	struct platform_device *pdev = NULL;
	struct regmap *spm_map, *infra_map, *smi_map;
	bool locked = false;
	int ret = -ENODEV;

	if (!gate || !dev)
		return -EINVAL;
	memset(gate, 0, sizeof(*gate));
	spin_lock_init(&gate->lock);
	provider = of_find_node_by_path(PROVIDER_PATH);
	spm = of_find_node_by_path(SPM_PATH);
	infra = of_find_node_by_path(INFRA_PATH);
	smi = of_find_node_by_path(SMI_PATH);
	if (!provider || !of_device_is_available(provider) ||
	    !of_device_is_compatible(provider,
				     "mediatek,mt8183-power-controller") ||
	    provider->parent != spm ||
	    of_find_property(provider, "access-controllers", NULL) ||
	    !fixed_resource(spm, "mediatek,mt8183-scpsys", 0x10006000) ||
	    !fixed_resource(infra, "mediatek,mt8183-infracfg", 0x10001000) ||
	    !fixed_resource(smi, "mediatek,mt8183-smi-common", 0x14019000) ||
	    !of_device_is_compatible(spm, "syscon") ||
	    !of_device_is_compatible(infra, "syscon") ||
	    of_device_is_compatible(smi, "syscon") ||
	    !provider_reference(provider, "mediatek,infracfg", infra) ||
	    !provider_reference(provider, "mediatek,smi", smi))
		goto out;

	pdev = of_find_device_by_node(provider);
	if (!pdev)
		goto out;
	if (!device_trylock(&pdev->dev)) {
		ret = -EBUSY;
		goto out;
	}
	locked = true;
	if (!pdev->dev.driver ||
	    strcmp(pdev->dev.driver->name, "mtk-power-controller") ||
	    pdev->dev.links.status != DL_DEV_DRIVER_BOUND)
		goto out;

	/* The fixed builtin provider cannot reach DRIVER_BOUND before obtaining
	 * all three maps above. Thus the get-or-create syscon APIs take their
	 * existing-map path here. There is no public existing-only API for a
	 * generic syscon. SMI is deliberately non-syscon: that lookup also
	 * refuses creation independently. The caller forbids provider removal
	 * and live-DT mutation for the entire resident experiment.
	 */
	smi_map = syscon_node_to_regmap(smi);
	if (IS_ERR(smi_map)) {
		ret = PTR_ERR(smi_map);
		goto out;
	}
	spm_map = syscon_node_to_regmap(spm);
	if (IS_ERR(spm_map)) {
		ret = PTR_ERR(spm_map);
		goto out;
	}
	infra_map = syscon_node_to_regmap(infra);
	if (IS_ERR(infra_map)) {
		ret = PTR_ERR(infra_map);
		goto out;
	}
	gate->spm = spm_map;
	gate->infra = infra_map;
	gate->smi = smi_map;
	ret = 0;
out:
	if (locked)
		device_unlock(&pdev->dev);
	if (pdev)
		put_device(&pdev->dev);
	of_node_put(smi);
	of_node_put(infra);
	of_node_put(spm);
	of_node_put(provider);
	if (ret)
		return ret;
	gate->dev = dev;
	gate->power_nb.notifier_call = power_event;
	ret = dev_pm_genpd_add_notifier(dev, &gate->power_nb);
	if (ret)
		return ret;
	gate->notifier_registered = true;
	gate->ready = true;
	return ret;
}

int mt8183_p1_stop_gate_arm(struct mt8183_p1_stop_gate *gate)
{
	unsigned long flags;
	int ret = 0;

	if (!gate || !gate->ready || !gate->notifier_registered)
		return -ENODEV;
	spin_lock_irqsave(&gate->lock, flags);
	if (gate->pending_off) {
		ret = -EBUSY;
	} else {
		gate->arm_seq = gate->off_seq;
		gate->provider_off = false;
		gate->armed = true;
		gate->event_seq++;
	}
	spin_unlock_irqrestore(&gate->lock, flags);
	return ret;
}

int mt8183_p1_stop_gate_cleanup(struct mt8183_p1_stop_gate *gate)
{
	int ret;

	if (!gate || !gate->notifier_registered)
		return 0;
	ret = dev_pm_genpd_remove_notifier(gate->dev);
	if (ret)
		return ret;
	/* Removal holds the domain lock and synchronizes the raw notifier. */
	gate->notifier_registered = false;
	gate->ready = false;
	return 0;
}

static int read_one(struct regmap *map, u32 offset, u32 *value,
		    struct mt8183_p1_stop_snapshot *snapshot, unsigned int index)
{
	unsigned int raw;
	int ret;

	ret = regmap_read(map, offset, &raw);
	if (ret) {
		snapshot->read_error = ret;
		return ret;
	}
	*value = raw;
	snapshot->valid_mask |= BIT(index);
	return 0;
}

int mt8183_p1_stop_gate_snapshot(struct mt8183_p1_stop_gate *gate,
			      struct mt8183_p1_stop_snapshot *snapshot)
{
	int ret;
	ktime_t start;
	unsigned long flags;
	u64 event_seq;

	if (!snapshot)
		return -EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	if (!gate || !gate->ready) {
		snapshot->read_error = -ENODEV;
		return -ENODEV;
	}
	start = ktime_get();
	spin_lock_irqsave(&gate->lock, flags);
	event_seq = gate->event_seq;
	spin_unlock_irqrestore(&gate->lock, flags);
	snapshot->samples = 1;
	ret = read_one(gate->spm, 0x180, &snapshot->pwr_status, snapshot, 0);
	if (ret)
		goto out;
	ret = read_one(gate->spm, 0x184, &snapshot->pwr_status_2nd, snapshot, 1);
	if (ret)
		goto out;
	ret = read_one(gate->spm, 0x344, &snapshot->cam_ctl, snapshot, 2);
	if (ret)
		goto out;
	ret = read_one(gate->infra, 0x2ec, &snapshot->infra_mm_sta, snapshot, 3);
	if (ret)
		goto out;
	ret = read_one(gate->infra, 0x228, &snapshot->infra_sta, snapshot, 4);
	if (ret)
		goto out;
	/* Refuse an SMI access with its parent DISP domain already unpowered.
	 * The caller's SMI runtime reference prevents an off transition during
	 * the read; these status checks alone cannot serialize power changes.
	 */
	if (!(snapshot->pwr_status & DISP_POWER_BIT) ||
	    !(snapshot->pwr_status_2nd & DISP_POWER_BIT)) {
		ret = -EHOSTDOWN;
		snapshot->read_error = ret;
		goto out;
	}
	ret = read_one(gate->smi, 0x3c0, &snapshot->smi_sta, snapshot, 5);
	if (ret)
		goto out;
	/* MT8183 coreboot infracfg register layout: EN 0x2d0, SET 0x2d4,
	 * CLR 0x2d8, STA0 0x2e8, STA1 0x2ec. Read-only retained request.
	 */
	ret = read_one(gate->infra, 0x2d0, &snapshot->infra_mm_en, snapshot, 6);
	if (ret)
		goto out;
	spin_lock_irqsave(&gate->lock, flags);
	snapshot->event_seq = gate->event_seq;
	snapshot->arm_seq = gate->arm_seq;
	snapshot->off_seq = gate->off_seq;
	snapshot->armed = gate->armed;
	snapshot->pending_off = gate->pending_off;
	snapshot->provider_off = gate->provider_off;
	spin_unlock_irqrestore(&gate->lock, flags);
	snapshot->passed = snapshot->valid_mask == VALID_ALL &&
		snapshot->event_seq == event_seq && snapshot->armed &&
		snapshot->provider_off && !snapshot->pending_off &&
		snapshot->off_seq > snapshot->arm_seq &&
		!(snapshot->pwr_status & CAM_POWER_BIT) &&
		!(snapshot->pwr_status_2nd & CAM_POWER_BIT) &&
		(snapshot->cam_ctl & CAM_CTL_MASK) == CAM_CTL_OFF &&
		(snapshot->infra_mm_en & INFRA_MM_MASK) == INFRA_MM_MASK &&
		(snapshot->infra_mm_sta & INFRA_MM_RETAINED_ACK_MASK) ==
			INFRA_MM_RETAINED_ACK_MASK &&
		(snapshot->infra_sta & INFRA_MASK) == INFRA_MASK &&
		(snapshot->smi_sta & SMI_MASK) == SMI_MASK;
out:
	snapshot->elapsed_us = ktime_us_delta(ktime_get(), start);
	return ret;
}

int mt8183_p1_stop_gate_wait(struct mt8183_p1_stop_gate *gate,
			  struct mt8183_p1_stop_snapshot *snapshot)
{
	ktime_t start = ktime_get();
	u32 samples = 0;
	int ret;

	if (!snapshot)
		return -EINVAL;
	for (;;) {
		ret = mt8183_p1_stop_gate_snapshot(gate, snapshot);
		snapshot->samples = ++samples;
		snapshot->elapsed_us = ktime_us_delta(ktime_get(), start);
		if (ret || snapshot->passed)
			return ret;
		if (snapshot->elapsed_us >= WAIT_US)
			return -ETIMEDOUT;
		usleep_range(10000, 12000);
	}
}
