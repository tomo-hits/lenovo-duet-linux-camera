/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef OV02A10_TELEMETRY_H
#define OV02A10_TELEMETRY_H

#include <linux/types.h>

#define OV02A10_RECORD_SCHEMA 2U
/* Finite diagnostic profile: orientation readback is observed separately. */
#define OV02A10_REQUIRED_MATCH_MASK 1791U
#define OV02A10_RECORD_READS 11U
#define OV02A10_RECORD_CONTROLS 4U

struct device;

enum ov02a10_record_phase {
	OV02A10_RECORD_NEVER_STARTED,
	OV02A10_RECORD_START_ATTEMPT,
	OV02A10_RECORD_PRE_STREAM_ON,
	OV02A10_RECORD_STREAM_STARTED,
	OV02A10_RECORD_START_FAILED,
};

/* Fixed indices: exposure H,L,H; gain; VTS H,L,H; pattern; orientation;
 * MIPI; standby. raw=-1/valid=0 represents unread, not a successful 0xff.
 */
struct ov02a10_record_read {
	u64 start_ns;
	u64 end_ns;
	s32 raw;
	s32 error;
	u32 attempted;
	u32 valid;
};

/* Fixed indices: exposure, analogue gain, VBLANK, test pattern.
 * Values are committed V4L2 cache codes, not application requests or
 * measured physical exposure/gain. All padding is initialized to zero.
 */
struct ov02a10_record_control {
	s64 minimum;
	s64 maximum;
	s64 step;
	s64 default_value;
	s32 value;
	u32 reserved;
};

struct ov02a10_start_record {
	u32 schema;
	u32 size;
	u64 probe_instance_ns;
	u64 epoch;
	u64 attempt_ns;
	u64 cache_ns;
	u64 read_start_ns;
	u64 read_end_ns;
	u64 stream_on_begin_ns;
	u64 stream_on_end_ns;
	u64 stop_ns;
	u64 page_select_start_ns;
	u64 page_select_end_ns;
	u64 page_restore_start_ns;
	u64 page_restore_end_ns;
	u32 phase;
	u32 attempt_valid;
	u32 epoch_overflow;
	u32 cache_valid;
	u32 width;
	u32 height;
	u32 media_bus_code;
	u32 rotation;
	u32 mipi_setting;
	u32 read_attempted_mask;
	u32 read_valid_mask;
	u32 pair_coherent_mask;
	u32 expected_match_mask;
	u32 readback_complete; /* all eleven match; not the v2 start gate */
	u32 required_match_mask;
	u32 required_readback_complete;
	u32 orientation_readback_match;
	u32 rotation_verified; /* optical orientation/Bayer remain unverified */
	s32 orientation_write_value; /* -1 before successful setup write */
	u32 stream_on_attempted;
	u32 stream_started; /* historical success, separate from stop_seen */
	u32 stop_seen;
	u32 cleanup_attempted;
	s32 first_error;
	s32 pm_get_error;
	s32 setup_error;
	s32 page_select_error;
	s32 page_restore_error;
	s32 stream_on_error;
	s32 cleanup_error;
	s32 stop_error;
	s32 pm_put_error;
	s32 exposure_raw;
	s32 vts_delta_raw;
	s32 driver_total_lines;
	struct ov02a10_record_control controls[OV02A10_RECORD_CONTROLS];
	struct ov02a10_record_read reads[OV02A10_RECORD_READS];
};

/* GPL-only, fixed-kernel internal API; no private subdev ioctl/UAPI.
 * Caller owns a valid get_device() reference obtained while the sensor
 * device was known to be alive. A device ref alone does not pin devm data:
 * the registry lock excludes lookup/removal; sensor mutex protects copy.
 * Lock order: caller's P1 process/control mutex -> registry -> sensor mutex.
 * Never call with sensor mutex/P1 spinlock, in IRQ, or from remove callbacks.
 * Remove unlinks/drains registry before sysfs removal or sensor-data teardown.
 * No I2C, PM resume, allocation, pointer loan, or new record is performed.
 * Return 0 also for an explicit NEVER_STARTED epoch0 record.
 * Wrong schema/size/device leaves *out unchanged. Caller module's normal
 * symbol dependency pins provider code, not the sensor instance. No force
 * unload/unbind during RAM capture; full active-remove safety is separate.
 */
int ov02a10_get_start_record(struct device *sensor_dev, u32 schema,
			   struct ov02a10_start_record *out, size_t out_size);

#endif
