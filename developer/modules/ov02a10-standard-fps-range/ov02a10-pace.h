/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef OV02A10_PACE_H
#define OV02A10_PACE_H

#include <linux/types.h>

#define OV02A10_PACE_SCHEMA 1U
#define OV02A10_PACE_START 1U
#define OV02A10_PACE_STOP 2U
#define OV02A10_PACE_CLEANUP 3U
#define OV02A10_PACE_US 250U
#define OV02A10_PACE_SLACK_US 100U

/* All times/durations are monotonic nanoseconds. Empty stats are all zero.
 * Overflow marks the phase unusable; it never prevents a cleanup transfer.
 */
struct ov02a10_pace_stat {
	u64 count;
	u64 min;
	u64 max;
	u64 sum;
};

/* 224 bytes; start and stop/cleanup have independent storage and clocks.
 * complete means the synchronous function returned, not PM suspended.
 * A failed bus call counts as an attempt, but not as a success.
 * wait excludes the first call; first_wait_ns records that call separately.
 */
struct ov02a10_pace_phase {
	u32 entered;
	u32 complete;
	u32 overflow;
	u32 kind;
	u32 first_wait_us;
	u32 between_wait_us;
	u32 attempts;
	u32 successes;
	s32 result;
	s32 first_bus_error;
	u32 reserved[2];
	u64 begin_ns;
	u64 end_ns;
	u64 first_bus_ns;
	u64 last_bus_begin_ns;
	u64 last_bus_end_ns;
	u64 first_wait_ns;
	struct ov02a10_pace_stat wait;
	struct ov02a10_pace_stat bus;
	struct ov02a10_pace_stat end_gap;
	struct ov02a10_pace_stat begin_gap;
};

/* 504 bytes, schema 1. Latest attempt only; no immutable slots or ack API.
 * Snapshot consumers must prevent another start until both records are saved.
 * attempt_valid=0 is probe-only. pm_complete means PM API returned (even error).
 * An unentered phase remains zero; no-op s_stream does not change any byte.
 */
struct ov02a10_pace_record {
	u32 schema;
	u32 size;
	u64 probe_instance_ns;
	u64 epoch;
	u32 attempt_valid;
	u32 epoch_overflow;
	u32 pm_attempted;
	u32 pm_complete;
	s32 pm_error;
	u32 reserved;
	u64 pm_return_ns;
	struct ov02a10_pace_phase start;
	struct ov02a10_pace_phase stop;
};

struct device;
int ov02a10_get_pace_record(struct device *sensor_dev, u32 schema,
			  struct ov02a10_pace_record *out, size_t out_size);

#endif
