/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT8183_P1_STOP_H
#define MT8183_P1_STOP_H

#include <linux/types.h>
#include <linux/notifier.h>
#include <linux/spinlock.h>

struct regmap;
struct device;

/* Borrowed, system-lifetime maps of the already-bound fixed power provider. */
struct mt8183_p1_stop_gate {
	struct regmap *spm;
	struct regmap *infra;
	struct regmap *smi;
	struct device *dev;
	struct notifier_block power_nb;
	spinlock_t lock;
	u64 event_seq, arm_seq, off_seq;
	bool armed, pending_off, provider_off, notifier_registered;
	bool ready;
};

/* valid_mask bits follow the seven raw fields below, starting at bit zero. */
struct mt8183_p1_stop_snapshot {
	u32 pwr_status;
	u32 pwr_status_2nd;
	u32 cam_ctl;
	u32 infra_mm_sta;
	u32 infra_sta;
	u32 smi_sta;
	u32 infra_mm_en;
	u32 valid_mask;
	u32 samples;
	u64 elapsed_us;
	u64 event_seq, arm_seq, off_seq;
	int read_error;
	bool armed, pending_off, provider_off;
	bool passed;
};

/* Invoke as the last fallible probe operation, while CAM is runtime-active. */
int mt8183_p1_stop_gate_init(struct mt8183_p1_stop_gate *gate, struct device *dev);
/* Arm exactly once before consuming an owned active CAM PM reference.
 * Do not re-arm a retry with pm_ref already consumed. PRE_ON invalidates the
 * witness, so each later active-to-off cycle requires a new arm.
 */
int mt8183_p1_stop_gate_arm(struct mt8183_p1_stop_gate *gate);
/* Unregister before releasing parent state; failure must retain that state. */
int mt8183_p1_stop_gate_cleanup(struct mt8183_p1_stop_gate *gate);
/* Caller serializes control, prevents a new CAM resume, and holds SMI common
 * runtime-active (DISP, not a CAM-domain larb) throughout either read method.
 * These methods do not stop SCP, change PM references, or grant buffer release.
 */
int mt8183_p1_stop_gate_snapshot(struct mt8183_p1_stop_gate *gate,
			      struct mt8183_p1_stop_snapshot *snapshot);
/* Sleepable, at most an 8-second polling interval plus final read/scheduling.
 * 0: all conditions observed; -ETIMEDOUT: readable but not stopped;
 * other negative errno: read/access error, recorded in read_error.
 */
int mt8183_p1_stop_gate_wait(struct mt8183_p1_stop_gate *gate,
			  struct mt8183_p1_stop_snapshot *snapshot);

#endif
