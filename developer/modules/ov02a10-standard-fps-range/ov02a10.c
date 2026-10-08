/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020 MediaTek Inc.

#include <linux/build_bug.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/units.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>
#include "ov02a10-telemetry.h"
#include "ov02a10-pace.h"

#define OV02A10_ID					0x2509
#define OV02A10_ID_MASK					GENMASK(15, 0)

#define OV02A10_REG_CHIP_ID				0x02

/* Bit[1] vertical upside down */
/* Bit[0] horizontal mirror */
#define REG_MIRROR_FLIP_CONTROL				0x3f

/* Orientation */
#define REG_MIRROR_FLIP_ENABLE				0x03

/* Bit[2:0] MIPI transmission speed select */
#define TX_SPEED_AREA_SEL				0xa1
#define OV02A10_MIPI_TX_SPEED_DEFAULT			0x04

#define REG_PAGE_SWITCH					0xfd
#define REG_GLOBAL_EFFECTIVE				0x01
#define REG_ENABLE					BIT(0)

#define REG_SC_CTRL_MODE				0xac
#define SC_CTRL_MODE_STANDBY				0x00
#define SC_CTRL_MODE_STREAMING				0x01

/* Exposure control */
#define OV02A10_EXP_SHIFT				8
#define OV02A10_REG_EXPOSURE_H				0x03
#define OV02A10_REG_EXPOSURE_L				0x04
#define	OV02A10_EXPOSURE_MIN				4
#define OV02A10_EXPOSURE_MAX_MARGIN			4
#define	OV02A10_EXPOSURE_STEP				1

/* Vblanking control */
#define OV02A10_VTS_SHIFT				8
#define OV02A10_REG_VTS_H				0x05
#define OV02A10_REG_VTS_L				0x06
#define OV02A10_VTS_MAX					0x209f
#define OV02A10_BASE_LINES				1224

/* Analog gain control */
#define OV02A10_REG_GAIN				0x24
#define OV02A10_GAIN_MIN				0x10
#define OV02A10_GAIN_MAX				0xf8
#define OV02A10_GAIN_STEP				0x01
#define OV02A10_GAIN_DEFAULT				0x40

/* Test pattern control */
#define OV02A10_REG_TEST_PATTERN			0xb6

#define OV02A10_LINK_FREQ_390MHZ			(390 * HZ_PER_MHZ)
/* Mode's documented 39MHz/934-tick line is normalized to the published
 * 78MHz sample domain: 1868/78MHz = 934/39MHz. At VTS2780 the measured
 * SOF period is 66.530ms vs model66.575ms (0.068%). Not a new HTS write.
 */
#define OV02A10_LINE_LENGTH 1868U
#define OV02A10_DEFAULT_VBLANK 1580U
/* Existing 1600x1200 mode: 1390 total lines, documented 30fps.
 * Keep the accepted 15fps default; expose its native minimum via V4L2. */
#define OV02A10_MIN_VBLANK 190U
#define OV02A10_ECLK_FREQ				(24 * HZ_PER_MHZ)

/* Number of lanes supported by this driver */
#define OV02A10_DATA_LANES				1

/* Bits per sample of sensor output */
#define OV02A10_BITS_PER_SAMPLE				10

static const char * const ov02a10_supply_names[] = {
	"dovdd",	/* Digital I/O power */
	"avdd",		/* Analog power */
	"dvdd",		/* Digital core power */
};

struct ov02a10_reg {
	u8 addr;
	u8 val;
};

struct ov02a10_reg_list {
	u32 num_of_regs;
	const struct ov02a10_reg *regs;
};

struct ov02a10_mode {
	u32 width;
	u32 height;
	u32 exp_def;
	u32 hts_def;
	u32 vts_def;
	const struct ov02a10_reg_list reg_list;
};

struct ov02a10 {
	struct device *dev;

	/* Indication of MIPI transmission speed select */
	u32 mipi_clock_voltage;

	struct clk *eclk;
	struct gpio_desc *pd_gpio;
	struct gpio_desc *rst_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(ov02a10_supply_names)];

	bool streaming;
	bool upside_down;

	/*
	 * Serialize control access, get/set format, get selection
	 * and start streaming.
	 */
	struct mutex mutex;
	struct v4l2_subdev subdev;
	struct media_pad pad;
	struct v4l2_mbus_framefmt fmt;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *gain;
	struct v4l2_ctrl *hflip, *vflip;
	struct v4l2_ctrl *flips[2];
	struct v4l2_ctrl *exposure_gain[2];
	struct ov02a10_pace_phase live_pace;
	u64 live_updates;
	int live_error;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *test_pattern;
	struct list_head telemetry_entry;
	struct ov02a10_start_record start_record;
	struct ov02a10_pace_record pace_record;
	struct ov02a10_pace_phase *pace_active;
	struct { u32 attempt; u8 page, reg, value; int result; } write_trace[16];
	u32 write_trace_count;
	u64 startup_settle_ns;
	u8 write_trace_page;
	u64 probe_instance_ns;
	u64 stream_attempt_epoch;

	const struct ov02a10_mode *cur_mode;
};

/* Protect devm-owned instances from exported getter/remove races. This lock
 * is never held while sysfs/async unregister waits or calls another driver.
 */
static DEFINE_MUTEX(ov02a10_record_registry_lock);
static LIST_HEAD(ov02a10_record_registry);

static_assert(sizeof(struct ov02a10_pace_record) == 504);
static_assert(sizeof(struct ov02a10_pace_phase) == 224);
static_assert(offsetof(struct ov02a10_pace_record, start) == 56);
static_assert(offsetof(struct ov02a10_pace_record, stop) == 280);

static const u8 ov02a10_record_registers[OV02A10_RECORD_READS] = {
	0x03, 0x04, 0x03, 0x24, 0x05, 0x06, 0x05, 0xb6, 0x3f, 0xa1, 0xac,
};

/* Pacing is active only under the sensor mutex during s_stream start/stop.
 * PM callbacks (including the word ID read) never use this byte wrapper.
 */
static void ov02a10_pace_clear(struct ov02a10 *sensor)
{
	struct ov02a10_pace_record *r = &sensor->pace_record;

	memset(r, 0, sizeof(*r));
	r->schema = OV02A10_PACE_SCHEMA;
	r->size = sizeof(*r);
	r->probe_instance_ns = sensor->probe_instance_ns;
	r->epoch = sensor->stream_attempt_epoch;
	sensor->pace_active = NULL;
}

static void ov02a10_pace_stat_add(struct ov02a10_pace_phase *p,
				struct ov02a10_pace_stat *s, u64 value)
{
	if (s->count == U64_MAX || value > U64_MAX - s->sum) {
		p->overflow = 1;
		return;
	}
	if (!s->count || value < s->min)
		s->min = value;
	if (!s->count || value > s->max)
		s->max = value;
	s->sum += value;
	s->count++;
}

static void ov02a10_pace_interval(struct ov02a10_pace_phase *p,
				struct ov02a10_pace_stat *s, u64 end, u64 begin)
{
	if (end < begin) {
		p->overflow = 1;
		return;
	}
	ov02a10_pace_stat_add(p, s, end - begin);
}

static void ov02a10_pace_begin(struct ov02a10 *sensor, u32 kind)
{
	struct ov02a10_pace_phase *p = kind == OV02A10_PACE_START ?
		&sensor->pace_record.start : &sensor->pace_record.stop;

	lockdep_assert_held(&sensor->mutex);
	/* Called exactly once per phase; never clear the other phase. */
	memset(p, 0, sizeof(*p));
	p->entered = 1;
	p->kind = kind;
	p->first_wait_us = kind == OV02A10_PACE_START ? 0 : OV02A10_PACE_US;
	p->between_wait_us = OV02A10_PACE_US;
	p->begin_ns = ktime_get_ns();
	sensor->pace_active = p;
}

static void ov02a10_pace_end(struct ov02a10 *sensor, int result)
{
	struct ov02a10_pace_phase *p = sensor->pace_active;

	lockdep_assert_held(&sensor->mutex);
	p->end_ns = ktime_get_ns();
	if (p->end_ns < p->begin_ns)
		p->overflow = 1;
	p->result = result;
	p->complete = 1;
	sensor->pace_active = NULL;
}

static int ov02a10_pace_bus(struct ov02a10 *sensor, bool read, u8 reg, u8 value)
{
	struct i2c_client *client = v4l2_get_subdevdata(&sensor->subdev);
	struct ov02a10_pace_phase *p = sensor->pace_active;
	u64 wait_begin, begin, end;
	u32 wait_us;
	int ret;

	/* Idle controls retain their original behavior and do not alter records. */
	if (!p)
		return read ? i2c_smbus_read_byte_data(client, reg) :
			i2c_smbus_write_byte_data(client, reg, value);
	lockdep_assert_held(&sensor->mutex);
	wait_us = p->attempts ? p->between_wait_us : p->first_wait_us;
	wait_begin = ktime_get_ns();
	if (wait_us)
		usleep_range(wait_us, wait_us + OV02A10_PACE_SLACK_US);
	begin = ktime_get_ns();
	ret = read ? i2c_smbus_read_byte_data(client, reg) :
		i2c_smbus_write_byte_data(client, reg, value);
	end = ktime_get_ns();
	/* RAM-only startup argument trace: no extra I2C or printk timing. */
	if (!read && p->kind == OV02A10_PACE_START) {
		if (reg == REG_PAGE_SWITCH && ret >= 0)
			sensor->write_trace_page = value;
		if ((reg == OV02A10_REG_EXPOSURE_H || reg == OV02A10_REG_EXPOSURE_L ||
		     reg == REG_GLOBAL_EFFECTIVE) && sensor->write_trace_count < 16) {
			u32 i = sensor->write_trace_count++;
			sensor->write_trace[i].attempt = p->attempts + 1;
			sensor->write_trace[i].page = sensor->write_trace_page;
			sensor->write_trace[i].reg = reg;
			sensor->write_trace[i].value = value;
			sensor->write_trace[i].result = ret;
		}
	}
	if (begin < wait_begin)
		p->overflow = 1;
	if (!p->attempts) {
		p->first_bus_ns = begin;
		p->first_wait_ns = begin >= wait_begin ? begin - wait_begin : 0;
	} else {
		ov02a10_pace_interval(p, &p->wait, begin, wait_begin);
		ov02a10_pace_interval(p, &p->end_gap, begin, p->last_bus_end_ns);
		ov02a10_pace_interval(p, &p->begin_gap, begin, p->last_bus_begin_ns);
	}
	ov02a10_pace_interval(p, &p->bus, end, begin);
	p->last_bus_begin_ns = begin;
	p->last_bus_end_ns = end;
	if (p->attempts == ~0U)
		p->overflow = 1;
	else
		p->attempts++;
	if (ret < 0 || (read && ret > 255)) {
		if (!p->first_bus_error)
			p->first_bus_error = ret < 0 ? ret : -EPROTO;
	} else if (p->successes == ~0U) {
		p->overflow = 1;
	} else {
		p->successes++;
	}
	/* Preserve transport return values; the readback caller checks >255. */
	return ret;
}

static void ov02a10_record_clear(struct ov02a10 *sensor)
{
	struct ov02a10_start_record *r = &sensor->start_record;
	unsigned int i;

	ov02a10_pace_clear(sensor);
	sensor->live_updates = 0;
	sensor->live_error = 0;
	memset(&sensor->live_pace, 0, sizeof(sensor->live_pace));
	sensor->write_trace_count = 0;
	sensor->startup_settle_ns = 0;
	sensor->write_trace_page = 0xff;
	memset(r, 0, sizeof(*r));
	r->schema = OV02A10_RECORD_SCHEMA;
	r->size = sizeof(*r);
	r->probe_instance_ns = sensor->probe_instance_ns;
	r->epoch = sensor->stream_attempt_epoch;
	r->exposure_raw = -1;
	r->vts_delta_raw = -1;
	r->driver_total_lines = -1;
	r->orientation_write_value = -1;
	r->required_match_mask = OV02A10_REQUIRED_MATCH_MASK;
	for (i = 0; i < OV02A10_RECORD_READS; i++)
		r->reads[i].raw = -1;
}

static void ov02a10_record_cache(struct ov02a10 *sensor)
{
	struct ov02a10_start_record *r = &sensor->start_record;
	struct v4l2_ctrl *ctrls[] = {
		sensor->exposure, sensor->gain, sensor->vblank, sensor->test_pattern,
	};
	unsigned int i;

	lockdep_assert_held(&sensor->mutex);
	for (i = 0; i < ARRAY_SIZE(ctrls); i++) {
		struct ov02a10_record_control *c = &r->controls[i];

		c->value = *ctrls[i]->p_cur.p_s32;
		c->minimum = ctrls[i]->minimum;
		c->maximum = ctrls[i]->maximum;
		c->step = ctrls[i]->step;
		c->default_value = ctrls[i]->default_value;
	}
	r->width = sensor->cur_mode->width;
	r->height = sensor->cur_mode->height;
	r->media_bus_code = sensor->fmt.code;
	r->rotation = sensor->upside_down ? 180 : 0;
	r->mipi_setting = sensor->mipi_clock_voltage;
	r->cache_ns = ktime_get_ns();
	r->cache_valid = 1;
}

static int ov02a10_record_begin(struct ov02a10 *sensor)
{
	struct ov02a10_start_record *r = &sensor->start_record;

	lockdep_assert_held(&sensor->mutex);
	ov02a10_record_clear(sensor);
	r->attempt_valid = 1;
	sensor->pace_record.attempt_valid = 1;
	r->attempt_ns = ktime_get_ns();
	r->phase = OV02A10_RECORD_START_ATTEMPT;
	ov02a10_record_cache(sensor);
	if (sensor->stream_attempt_epoch == U64_MAX) {
		r->epoch_overflow = 1;
		sensor->pace_record.epoch_overflow = 1;
		r->first_error = -EOVERFLOW;
		r->phase = OV02A10_RECORD_START_FAILED;
		return -EOVERFLOW;
	}
	r->epoch = ++sensor->stream_attempt_epoch;
	sensor->pace_record.epoch = r->epoch;
	return 0;
}

static int ov02a10_record_validate(struct ov02a10_start_record *r)
{
	u32 exposure = r->controls[0].value;
	u32 vts = r->controls[2].value + r->height - OV02A10_BASE_LINES;
	u8 expected[OV02A10_RECORD_READS] = {
		exposure >> 8, exposure, exposure >> 8, r->controls[1].value,
		vts >> 8, vts, vts >> 8, r->controls[3].value,
		r->orientation_write_value, r->mipi_setting, 0,
	};
	unsigned int i;

	if (r->read_valid_mask != BIT(OV02A10_RECORD_READS) - 1)
		return -ENODATA;
	for (i = 0; i < OV02A10_RECORD_READS; i++)
		if (r->reads[i].raw == expected[i])
			r->expected_match_mask |= BIT(i);
	if (r->reads[0].raw == r->reads[2].raw) {
		r->pair_coherent_mask |= BIT(0);
		r->exposure_raw = r->reads[0].raw << 8 | r->reads[1].raw;
	}
	if (r->reads[4].raw == r->reads[6].raw) {
		r->pair_coherent_mask |= BIT(1);
		r->vts_delta_raw = r->reads[4].raw << 8 | r->reads[5].raw;
		r->driver_total_lines = r->vts_delta_raw + OV02A10_BASE_LINES;
	}
	if (r->pair_coherent_mask != 3)
		return -EAGAIN;
	r->orientation_readback_match = !!(r->expected_match_mask & BIT(8));
	/* Register 0x3f readback did not match the acknowledged write on the
	 * real sensor. Preserve that mismatch; this finite diagnostic profile
	 * verifies the other ten values without claiming orientation or Bayer.
	 * Unknown orientation values still reject start. No write is changed.
	 */
	if (r->reads[8].raw > 3)
		return -EUCLEAN;
	return r->expected_match_mask == BIT(OV02A10_RECORD_READS) - 1 ? 0 : -EUCLEAN;
}

static int ov02a10_record_readback(struct ov02a10 *sensor)
{
	struct ov02a10_start_record *r = &sensor->start_record;
	unsigned int i;
	int ret;

	lockdep_assert_held(&sensor->mutex);
	/* Called only after successful stream PM resume/control setup, while
	 * the stream reference is held. Never acquire another PM reference.
	 */
	if (!pm_runtime_active(sensor->dev))
		return -EHOSTDOWN;
	ov02a10_record_cache(sensor);
	r->phase = OV02A10_RECORD_PRE_STREAM_ON;
	r->read_start_ns = ktime_get_ns();
	r->page_select_start_ns = ktime_get_ns();
	ret = ov02a10_pace_bus(sensor, false, REG_PAGE_SWITCH, REG_ENABLE);
	r->page_select_end_ns = ktime_get_ns();
	r->page_select_error = ret < 0 ? ret : 0;
	if (ret >= 0) {
		for (i = 0; i < OV02A10_RECORD_READS; i++) {
			struct ov02a10_record_read *read = &r->reads[i];

			read->attempted = 1;
			r->read_attempted_mask |= BIT(i);
			read->start_ns = ktime_get_ns();
			ret = ov02a10_pace_bus(sensor, true, ov02a10_record_registers[i], 0);
			read->end_ns = ktime_get_ns();
			if (ret < 0 || ret > 255) {
				read->error = ret < 0 ? ret : -EPROTO;
				ret = read->error;
				break;
			}
			read->raw = ret;
			read->valid = 1;
			r->read_valid_mask |= BIT(i);
		}
		if (ret >= 0)
			ret = ov02a10_record_validate(r);
	}
	/* Always restore the known entry page, including a failed select/read.
	 * The original primary error remains separate from restore failure.
	 */
	if (ret < 0 && !r->first_error)
		r->first_error = ret;
	r->page_restore_start_ns = ktime_get_ns();
	ret = ov02a10_pace_bus(sensor, false, REG_PAGE_SWITCH, REG_ENABLE);
	r->page_restore_end_ns = ktime_get_ns();
	r->page_restore_error = ret < 0 ? ret : 0;
	if (ret < 0 && !r->first_error)
		r->first_error = ret;
	r->read_end_ns = ktime_get_ns();
	r->required_readback_complete = !r->first_error;
	r->readback_complete = !r->first_error &&
		r->expected_match_mask == BIT(OV02A10_RECORD_READS) - 1;
	return r->first_error;
}

static void ov02a10_record_register(struct ov02a10 *sensor)
{
	mutex_lock(&ov02a10_record_registry_lock);
	list_add_tail(&sensor->telemetry_entry, &ov02a10_record_registry);
	mutex_unlock(&ov02a10_record_registry_lock);
}

static void ov02a10_record_unregister(struct ov02a10 *sensor)
{
	/* Taking this lock waits for any getter's sensor copy to finish. */
	mutex_lock(&ov02a10_record_registry_lock);
	list_del_init(&sensor->telemetry_entry);
	mutex_unlock(&ov02a10_record_registry_lock);
}

int ov02a10_get_start_record(struct device *sensor_dev, u32 schema,
			   struct ov02a10_start_record *out, size_t out_size)
{
	struct ov02a10 *sensor;
	int ret = -ENODEV;

	if (!sensor_dev || !out)
		return -EINVAL;
	if (schema != OV02A10_RECORD_SCHEMA)
		return -EPROTONOSUPPORT;
	if (out_size != sizeof(*out))
		return -EMSGSIZE;
	mutex_lock(&ov02a10_record_registry_lock);
	list_for_each_entry(sensor, &ov02a10_record_registry, telemetry_entry) {
		if (sensor->dev != sensor_dev)
			continue;
		mutex_lock(&sensor->mutex);
		*out = sensor->start_record;
		mutex_unlock(&sensor->mutex);
		ret = 0;
		break;
	}
	mutex_unlock(&ov02a10_record_registry_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(ov02a10_get_start_record);

int ov02a10_get_pace_record(struct device *sensor_dev, u32 schema,
			   struct ov02a10_pace_record *out, size_t out_size)
{
	struct ov02a10 *sensor;
	int ret = -ENODEV;

	if (!sensor_dev || !out)
		return -EINVAL;
	if (schema != OV02A10_PACE_SCHEMA)
		return -EPROTONOSUPPORT;
	if (out_size != sizeof(*out))
		return -EMSGSIZE;
	mutex_lock(&ov02a10_record_registry_lock);
	list_for_each_entry(sensor, &ov02a10_record_registry, telemetry_entry) {
		if (sensor->dev != sensor_dev)
			continue;
		mutex_lock(&sensor->mutex);
		*out = sensor->pace_record;
		mutex_unlock(&sensor->mutex);
		ret = 0;
		break;
	}
	mutex_unlock(&ov02a10_record_registry_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(ov02a10_get_pace_record);

static ssize_t ov02a10_record_format(char *buf,
				   const struct ov02a10_start_record *r)
{
	ssize_t n = 0;
	unsigned int i;

#define RU(name) n += sysfs_emit_at(buf, n, #name "=%u ", r->name)
#define RS(name) n += sysfs_emit_at(buf, n, #name "=%d ", r->name)
#define RT(name) n += sysfs_emit_at(buf, n, #name "=%llu ", (unsigned long long)r->name)
	RU(schema); RU(size); RT(probe_instance_ns); RT(epoch);
	RU(phase); RU(attempt_valid); RU(epoch_overflow); RU(cache_valid);
	RT(attempt_ns); RT(cache_ns); RT(read_start_ns); RT(read_end_ns);
	RT(stream_on_begin_ns); RT(stream_on_end_ns); RT(stop_ns);
	RT(page_select_start_ns); RT(page_select_end_ns);
	RT(page_restore_start_ns); RT(page_restore_end_ns);
	RU(width); RU(height); RU(media_bus_code); RU(rotation); RU(mipi_setting);
	RU(read_attempted_mask); RU(read_valid_mask); RU(pair_coherent_mask);
	RU(expected_match_mask); RU(readback_complete); RU(stream_on_attempted);
	RU(required_match_mask); RU(required_readback_complete);
	RU(orientation_readback_match); RU(rotation_verified);
	RS(orientation_write_value);
	RU(stream_started); RU(stop_seen); RU(cleanup_attempted);
	RS(first_error); RS(pm_get_error); RS(setup_error);
	RS(page_select_error); RS(page_restore_error); RS(stream_on_error);
	RS(cleanup_error); RS(stop_error); RS(pm_put_error);
	RS(exposure_raw); RS(vts_delta_raw); RS(driver_total_lines);
#undef RU
#undef RS
#undef RT
	for (i = 0; i < OV02A10_RECORD_CONTROLS; i++) {
		const struct ov02a10_record_control *c = &r->controls[i];

		n += sysfs_emit_at(buf, n, "c%u_value=%d c%u_min=%lld c%u_max=%lld ",
			i, c->value, i, (long long)c->minimum, i, (long long)c->maximum);
		n += sysfs_emit_at(buf, n, "c%u_step=%lld c%u_default=%lld ",
			i, (long long)c->step, i, (long long)c->default_value);
	}
	for (i = 0; i < OV02A10_RECORD_READS; i++) {
		const struct ov02a10_record_read *read = &r->reads[i];

		n += sysfs_emit_at(buf, n, "r%u_page=1 r%u_reg=%02x r%u_raw=%d r%u_errno=%d ",
			i, i, ov02a10_record_registers[i], i, read->raw, i, read->error);
		n += sysfs_emit_at(buf, n, "r%u_start_ns=%llu r%u_end_ns=%llu ",
			i, (unsigned long long)read->start_ns, i, (unsigned long long)read->end_ns);
	}
	n += sysfs_emit_at(buf, n, "clock=monotonic applied_frame=unknown requested=not_recorded\n");
	return n;
}

static ssize_t telemetry_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct ov02a10_start_record record;
	int ret;

	ret = ov02a10_get_start_record(dev, OV02A10_RECORD_SCHEMA,
				     &record, sizeof(record));
	if (ret)
		return ret;
	return ov02a10_record_format(buf, &record);
}
static DEVICE_ATTR_RO(telemetry);

static ssize_t ov02a10_pace_phase_format(char *buf, ssize_t n, const char *prefix,
				       const struct ov02a10_pace_phase *p)
{
#define PU(name) n += sysfs_emit_at(buf, n, "%s_" #name "=%u ", prefix, p->name)
#define PS(name) n += sysfs_emit_at(buf, n, "%s_" #name "=%d ", prefix, p->name)
#define PT(name) n += sysfs_emit_at(buf, n, "%s_" #name "=%llu ", prefix, (unsigned long long)p->name)
#define STAT(name) n += sysfs_emit_at(buf, n, "%s_" #name "=%llu,%llu,%llu,%llu ", prefix, \
	(unsigned long long)p->name.count, (unsigned long long)p->name.min, \
	(unsigned long long)p->name.max, (unsigned long long)p->name.sum)
	PU(entered); PU(complete); PU(overflow); PU(kind);
	PU(first_wait_us); PU(between_wait_us); PU(attempts); PU(successes);
	PS(result); PS(first_bus_error);
	PT(begin_ns); PT(end_ns); PT(first_bus_ns);
	PT(last_bus_begin_ns); PT(last_bus_end_ns); PT(first_wait_ns);
	STAT(wait); STAT(bus); STAT(end_gap); STAT(begin_gap);
#undef PU
#undef PS
#undef PT
#undef STAT
	return n;
}

static ssize_t ov02a10_pace_format(char *buf, const struct ov02a10_pace_record *r)
{
	ssize_t n = 0;

#define PU(name) n += sysfs_emit_at(buf, n, #name "=%u ", r->name)
#define PT(name) n += sysfs_emit_at(buf, n, #name "=%llu ", (unsigned long long)r->name)
	PU(schema); PU(size); PT(probe_instance_ns); PT(epoch);
	PU(attempt_valid); PU(epoch_overflow); PU(pm_attempted); PU(pm_complete);
	n += sysfs_emit_at(buf, n, "pm_error=%d ", r->pm_error);
	PT(pm_return_ns);
#undef PU
#undef PT
	n = ov02a10_pace_phase_format(buf, n, "start", &r->start);
	n = ov02a10_pace_phase_format(buf, n, "stop", &r->stop);
	n += sysfs_emit_at(buf, n, "clock=monotonic stats=count,min,max,sum unit=ns\n");
	return n;
}

static ssize_t pace_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ov02a10_pace_record record;
	int ret;

	ret = ov02a10_get_pace_record(dev, OV02A10_PACE_SCHEMA, &record, sizeof(record));
	if (ret)
		return ret;
	return ov02a10_pace_format(buf, &record);
}
static DEVICE_ATTR_RO(pace);

static ssize_t startup_writes_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov02a10 *sensor = container_of(sd, struct ov02a10, subdev);
	ssize_t n;
	u32 i;
	mutex_lock(&sensor->mutex);
	n = sysfs_emit_at(buf, 0, "probe=%llu epoch=%llu count=%u cache_exposure=%d settle_ns=%llu\n",
		(unsigned long long)sensor->probe_instance_ns,
		(unsigned long long)sensor->stream_attempt_epoch,
		sensor->write_trace_count, *sensor->exposure->p_cur.p_s32,
		(unsigned long long)sensor->startup_settle_ns);
	for (i = 0; i < sensor->write_trace_count; i++)
		n += sysfs_emit_at(buf, n, "attempt=%u page=%u reg=%02x value=%u ret=%d\n",
			sensor->write_trace[i].attempt, sensor->write_trace[i].page,
			sensor->write_trace[i].reg, sensor->write_trace[i].value,
			sensor->write_trace[i].result);
	mutex_unlock(&sensor->mutex);
	return n;
}
static DEVICE_ATTR_RO(startup_writes);

static ssize_t live_controls_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
	struct ov02a10 *sensor = container_of(sd, struct ov02a10, subdev);
	ssize_t n;
	(void)attr;
	mutex_lock(&sensor->mutex);
	n = sysfs_emit(buf, "epoch=%llu updates=%llu error=%d attempts=%u write_duration_ns=%llu exposure_cache=%d gain_cache=%d optical_delay=unverified\n",
		(unsigned long long)sensor->stream_attempt_epoch,
		(unsigned long long)sensor->live_updates, sensor->live_error,
		sensor->live_pace.attempts,
		(unsigned long long)(sensor->live_pace.end_ns - sensor->live_pace.begin_ns),
		*sensor->exposure->p_cur.p_s32, *sensor->gain->p_cur.p_s32);
	mutex_unlock(&sensor->mutex);
	return n;
}
static DEVICE_ATTR_RO(live_controls);

static struct attribute *ov02a10_telemetry_attrs[] = {
	&dev_attr_telemetry.attr,
	&dev_attr_pace.attr,
	&dev_attr_startup_writes.attr,
	&dev_attr_live_controls.attr,
	NULL,
};

static const struct attribute_group ov02a10_telemetry_group = {
	.attrs = ov02a10_telemetry_attrs,
};

static int ov02a10_record_publish(struct ov02a10 *sensor)
{
	int ret;

	ov02a10_record_register(sensor);
	ret = sysfs_create_group(&sensor->dev->kobj, &ov02a10_telemetry_group);
	if (ret)
		ov02a10_record_unregister(sensor);
	return ret;
}

static void ov02a10_record_unpublish(struct ov02a10 *sensor)
{
	ov02a10_record_unregister(sensor);
	/* sysfs removal may wait for a show/getter; hold neither mutex. */
	sysfs_remove_group(&sensor->dev->kobj, &ov02a10_telemetry_group);
}

static inline struct ov02a10 *to_ov02a10(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov02a10, subdev);
}

/*
 * eclk 24Mhz
 * pclk 39Mhz
 * linelength 934(0x3a6)
 * framelength 1390(0x56E)
 * grabwindow_width 1600
 * grabwindow_height 1200
 * max_framerate 30fps
 * mipi_datarate per lane 780Mbps
 */
static const struct ov02a10_reg ov02a10_1600x1200_regs[] = {
	{0xfd, 0x01},
	{0xac, 0x00},
	{0xfd, 0x00},
	{0x2f, 0x29},
	{0x34, 0x00},
	{0x35, 0x21},
	{0x30, 0x15},
	{0x33, 0x01},
	{0xfd, 0x01},
	{0x44, 0x00},
	{0x2a, 0x4c},
	{0x2b, 0x1e},
	{0x2c, 0x60},
	{0x25, 0x11},
	{0x03, 0x01},
	{0x04, 0xae},
	{0x09, 0x00},
	{0x0a, 0x02},
	{0x06, 0xa6},
	{0x31, 0x00},
	{0x24, 0x40},
	{0x01, 0x01},
	{0xfb, 0x73},
	{0xfd, 0x01},
	{0x16, 0x04},
	{0x1c, 0x09},
	{0x21, 0x42},
	{0x12, 0x04},
	{0x13, 0x10},
	{0x11, 0x40},
	{0x33, 0x81},
	{0xd0, 0x00},
	{0xd1, 0x01},
	{0xd2, 0x00},
	{0x50, 0x10},
	{0x51, 0x23},
	{0x52, 0x20},
	{0x53, 0x10},
	{0x54, 0x02},
	{0x55, 0x20},
	{0x56, 0x02},
	{0x58, 0x48},
	{0x5d, 0x15},
	{0x5e, 0x05},
	{0x66, 0x66},
	{0x68, 0x68},
	{0x6b, 0x00},
	{0x6c, 0x00},
	{0x6f, 0x40},
	{0x70, 0x40},
	{0x71, 0x0a},
	{0x72, 0xf0},
	{0x73, 0x10},
	{0x75, 0x80},
	{0x76, 0x10},
	{0x84, 0x00},
	{0x85, 0x10},
	{0x86, 0x10},
	{0x87, 0x00},
	{0x8a, 0x22},
	{0x8b, 0x22},
	{0x19, 0xf1},
	{0x29, 0x01},
	{0xfd, 0x01},
	{0x9d, 0x16},
	{0xa0, 0x29},
	{0xa1, 0x04},
	{0xad, 0x62},
	{0xae, 0x00},
	{0xaf, 0x85},
	{0xb1, 0x01},
	{0x8e, 0x06},
	{0x8f, 0x40},
	{0x90, 0x04},
	{0x91, 0xb0},
	{0x45, 0x01},
	{0x46, 0x00},
	{0x47, 0x6c},
	{0x48, 0x03},
	{0x49, 0x8b},
	{0x4a, 0x00},
	{0x4b, 0x07},
	{0x4c, 0x04},
	{0x4d, 0xb7},
	{0xf0, 0x40},
	{0xf1, 0x40},
	{0xf2, 0x40},
	{0xf3, 0x40},
	{0x3f, 0x00},
	{0xfd, 0x01},
	{0x05, 0x00},
	{0x06, 0xa6},
	{0xfd, 0x01},
};

static const char * const ov02a10_test_pattern_menu[] = {
	"Disabled",
	"Eight Vertical Colour Bars",
};

static const s64 link_freq_menu_items[] = {
	OV02A10_LINK_FREQ_390MHZ,
};

static u64 to_pixel_rate(u32 f_index)
{
	u64 pixel_rate = link_freq_menu_items[f_index] * 2 * OV02A10_DATA_LANES;

	do_div(pixel_rate, OV02A10_BITS_PER_SAMPLE);

	return pixel_rate;
}

static const struct ov02a10_mode supported_modes[] = {
	{
		.width = 1600,
		.height = 1200,
		.exp_def = 0x01ae,
		.hts_def = 0x03a6,
		.vts_def = 0x056e,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(ov02a10_1600x1200_regs),
			.regs = ov02a10_1600x1200_regs,
		},
	},
};

static int ov02a10_write_array(struct ov02a10 *ov02a10,
			       const struct ov02a10_reg_list *r_list)
{
	unsigned int i;
	int ret;

	for (i = 0; i < r_list->num_of_regs; i++) {
		ret = ov02a10_pace_bus(ov02a10, false, r_list->regs[i].addr,
						r_list->regs[i].val);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/* Native BGGR; mirror reverses the even-width CFA columns, flip its rows.
 * Physical mounting rotation stays a firmware property, separate from controls. */
static u32 ov02a10_bayer_code(struct ov02a10 *s)
{
 static const u32 codes[] = { MEDIA_BUS_FMT_SBGGR10_1X10,
  MEDIA_BUS_FMT_SGBRG10_1X10, MEDIA_BUS_FMT_SGRBG10_1X10,
  MEDIA_BUS_FMT_SRGGB10_1X10 };
 return codes[s->hflip->val | (s->vflip->val << 1)];
}

static void ov02a10_fill_fmt(const struct ov02a10_mode *mode,
			     struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
}

static int ov02a10_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct ov02a10 *ov02a10 = to_ov02a10(sd);
	struct v4l2_mbus_framefmt *mbus_fmt = &fmt->format;
	struct v4l2_mbus_framefmt *frame_fmt;
	int ret = 0;

	mutex_lock(&ov02a10->mutex);

	if (ov02a10->streaming && fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		ret = -EBUSY;
		goto out_unlock;
	}

	/* Only one sensor mode supported */
	mbus_fmt->code = ov02a10_bayer_code(ov02a10);
	ov02a10_fill_fmt(ov02a10->cur_mode, mbus_fmt);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		frame_fmt = v4l2_subdev_state_get_format(sd_state, 0);
	else
		frame_fmt = &ov02a10->fmt;

	*frame_fmt = *mbus_fmt;

out_unlock:
	mutex_unlock(&ov02a10->mutex);
	return ret;
}

static int ov02a10_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct ov02a10 *ov02a10 = to_ov02a10(sd);
	struct v4l2_mbus_framefmt *mbus_fmt = &fmt->format;

	mutex_lock(&ov02a10->mutex);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		fmt->format = *v4l2_subdev_state_get_format(sd_state,
							    fmt->pad);
	} else {
		fmt->format = ov02a10->fmt;
		mbus_fmt->code = ov02a10_bayer_code(ov02a10);
		ov02a10_fill_fmt(ov02a10->cur_mode, mbus_fmt);
	}

	mutex_unlock(&ov02a10->mutex);

	return 0;
}

static int ov02a10_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	struct ov02a10 *ov02a10 = to_ov02a10(sd);

	if (code->index != 0)
		return -EINVAL;

	mutex_lock(&ov02a10->mutex);
	code->code = ov02a10_bayer_code(ov02a10);
	mutex_unlock(&ov02a10->mutex);

	return 0;
}

static int ov02a10_enum_frame_sizes(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	fse->min_width  = supported_modes[fse->index].width;
	fse->max_width  = supported_modes[fse->index].width;
	fse->max_height = supported_modes[fse->index].height;
	fse->min_height = supported_modes[fse->index].height;

	return 0;
}

static int ov02a10_check_sensor_id(struct ov02a10 *ov02a10)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov02a10->subdev);
	u16 chip_id;
	int ret;

	/* Validate the chip ID */
	ret = i2c_smbus_read_word_swapped(client, OV02A10_REG_CHIP_ID);
	if (ret < 0)
		return ret;

	chip_id = le16_to_cpu((__force __le16)ret);

	if ((chip_id & OV02A10_ID_MASK) != OV02A10_ID) {
		dev_err(ov02a10->dev, "unexpected sensor id(0x%04x)\n", chip_id);
		return -EINVAL;
	}

	dev_info(ov02a10->dev, "sensor ID verified: 0x%04x\n", chip_id);

	return 0;
}

static int ov02a10_power_on(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov02a10 *ov02a10 = to_ov02a10(sd);
	int ret;

	gpiod_set_value_cansleep(ov02a10->rst_gpio, 1);
	gpiod_set_value_cansleep(ov02a10->pd_gpio, 1);

	ret = clk_prepare_enable(ov02a10->eclk);
	if (ret < 0) {
		dev_err(dev, "failed to enable eclk\n");
		return ret;
	}

	ret = regulator_bulk_enable(ARRAY_SIZE(ov02a10_supply_names),
				    ov02a10->supplies);
	if (ret < 0) {
		dev_err(dev, "failed to enable regulators\n");
		goto disable_clk;
	}
	usleep_range(5000, 6000);

	gpiod_set_value_cansleep(ov02a10->pd_gpio, 0);
	usleep_range(5000, 6000);

	gpiod_set_value_cansleep(ov02a10->rst_gpio, 0);
	usleep_range(5000, 6000);

	ret = ov02a10_check_sensor_id(ov02a10);
	if (ret)
		goto disable_regulator;

	return 0;

disable_regulator:
	regulator_bulk_disable(ARRAY_SIZE(ov02a10_supply_names),
			       ov02a10->supplies);
disable_clk:
	clk_disable_unprepare(ov02a10->eclk);

	return ret;
}

static int ov02a10_power_off(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov02a10 *ov02a10 = to_ov02a10(sd);

	gpiod_set_value_cansleep(ov02a10->rst_gpio, 1);
	clk_disable_unprepare(ov02a10->eclk);
	gpiod_set_value_cansleep(ov02a10->pd_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ov02a10_supply_names),
			       ov02a10->supplies);

	return 0;
}

static int __ov02a10_start_stream(struct ov02a10 *ov02a10)
{
	const struct ov02a10_reg_list *reg_list;
	struct ov02a10_start_record *r = &ov02a10->start_record;
	int ret;

	/* Apply default values of current mode */
	reg_list = &ov02a10->cur_mode->reg_list;
	ret = ov02a10_write_array(ov02a10, reg_list);
	if (ret)
		goto setup_fail;

	/* The successful mode table wrote orientation 0 on page 1. */
	r->orientation_write_value = 0;

	/* Apply customized values from user */
	ret = __v4l2_ctrl_handler_setup(ov02a10->subdev.ctrl_handler);
	if (ret)
		goto setup_fail;


	/* Set MIPI TX speed according to DT property */
	if (ov02a10->mipi_clock_voltage != OV02A10_MIPI_TX_SPEED_DEFAULT) {
		ret = ov02a10_pace_bus(ov02a10, false, TX_SPEED_AREA_SEL,
						ov02a10->mipi_clock_voltage);
		if (ret < 0)
			goto setup_fail;
	}

	/* Candidate: allow latched timing/exposure registers to settle before
	 * the one strict readback. No extra writes or readback retries. */
	{
		u64 begin = ktime_get_ns();
		usleep_range(140000, 141000);
		ov02a10->startup_settle_ns = ktime_get_ns() - begin;
	}
	ret = ov02a10_record_readback(ov02a10);
	if (ret)
		return ret;
	if (ov02a10->pace_record.start.overflow)
		return -EOVERFLOW;
	r->stream_on_attempted = 1;
	r->stream_on_begin_ns = ktime_get_ns();
	ret = ov02a10_pace_bus(ov02a10, false, REG_SC_CTRL_MODE,
					SC_CTRL_MODE_STREAMING);
	r->stream_on_end_ns = ktime_get_ns();
	r->stream_on_error = ret < 0 ? ret : 0;
	return ret;

setup_fail:
	r->setup_error = ret;
	return ret;
}

static int __ov02a10_stop_stream(struct ov02a10 *ov02a10)
{
	int ret;

	/* A failed mode-table write may have left the sensor on page 0. */
	ret = ov02a10_pace_bus(ov02a10, false, REG_PAGE_SWITCH, REG_ENABLE);
	if (ret < 0)
		return ret;

	return ov02a10_pace_bus(ov02a10, false, REG_SC_CTRL_MODE,
					 SC_CTRL_MODE_STANDBY);
}

static int ov02a10_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.format = {
			.width = 1600,
			.height = 1200,
		}
	};

	ov02a10_set_fmt(sd, sd_state, &fmt);

	return 0;
}

static int ov02a10_s_stream(struct v4l2_subdev *sd, int on)
{
	struct ov02a10 *ov02a10 = to_ov02a10(sd);
	struct ov02a10_start_record *r = &ov02a10->start_record;
	int ret, cleanup_ret;

	mutex_lock(&ov02a10->mutex);
	if (ov02a10->streaming == on) {
		ret = 0;
		goto unlock_and_return;
	}
	if (on) {
		ret = ov02a10_record_begin(ov02a10);
		if (ret)
			goto unlock_and_return;
		ov02a10->pace_record.pm_attempted = 1;
		ret = pm_runtime_resume_and_get(ov02a10->dev);
		ov02a10->pace_record.pm_return_ns = ktime_get_ns();
		ov02a10->pace_record.pm_complete = 1;
		ov02a10->pace_record.pm_error = ret < 0 ? ret : 0;
		if (ret < 0) {
			r->pm_get_error = ret;
			goto failed_no_put;
		}
		ov02a10_pace_begin(ov02a10, OV02A10_PACE_START);
		ret = __ov02a10_start_stream(ov02a10);
		ov02a10_pace_end(ov02a10, ret);
		if (!ret && ov02a10->pace_record.start.overflow) {
			ret = -EOVERFLOW;
			ov02a10->pace_record.start.result = ret;
		}
		if (ret) {
			r->cleanup_attempted = 1;
			ov02a10_pace_begin(ov02a10, OV02A10_PACE_CLEANUP);
			cleanup_ret = __ov02a10_stop_stream(ov02a10);
			ov02a10_pace_end(ov02a10, cleanup_ret);
			r->cleanup_error = cleanup_ret < 0 ? cleanup_ret : 0;
			ov02a10->streaming = false;
			cleanup_ret = pm_runtime_put(ov02a10->dev);
			r->pm_put_error = cleanup_ret < 0 ? cleanup_ret : 0;
			goto failed_no_put;
		}
		r->stream_started = 1;
		r->phase = OV02A10_RECORD_STREAM_STARTED;
		ov02a10->streaming = true;
  __v4l2_ctrl_grab(ov02a10->hflip, true);
  __v4l2_ctrl_grab(ov02a10->vflip, true);
	} else {
  __v4l2_ctrl_grab(ov02a10->hflip, false);
  __v4l2_ctrl_grab(ov02a10->vflip, false);
		r->stop_seen = 1;
		r->stop_ns = ktime_get_ns();
		ov02a10_pace_begin(ov02a10, OV02A10_PACE_STOP);
		ret = __ov02a10_stop_stream(ov02a10);
		ov02a10_pace_end(ov02a10, ret);
		r->stop_error = ret < 0 ? ret : 0;
		/* Consume one stream reference even when standby fails. */
		ov02a10->streaming = false;
		cleanup_ret = pm_runtime_put(ov02a10->dev);
		r->pm_put_error = cleanup_ret < 0 ? cleanup_ret : 0;
		if (!ret && cleanup_ret < 0)
			ret = cleanup_ret;
	}
	goto unlock_and_return;

failed_no_put:
	if (!r->first_error)
		r->first_error = ret;
	r->phase = OV02A10_RECORD_START_FAILED;
unlock_and_return:
	mutex_unlock(&ov02a10->mutex);
	return ret;
}

static const struct dev_pm_ops ov02a10_pm_ops = {
	SET_RUNTIME_PM_OPS(ov02a10_power_off, ov02a10_power_on, NULL)
};

/* Exposure and analogue gain are one standard V4L2 control cluster. */
static int ov02a10_set_exposure_gain(struct ov02a10 *sensor)
{
	const u8 regs[] = { REG_PAGE_SWITCH, OV02A10_REG_EXPOSURE_H,
		OV02A10_REG_EXPOSURE_L, OV02A10_REG_GAIN, REG_GLOBAL_EFFECTIVE };
	const u8 vals[] = { REG_ENABLE, sensor->exposure->val >> 8,
		sensor->exposure->val, sensor->gain->val, REG_ENABLE };
	unsigned int i;
	int ret = 0, restore;
	bool live = sensor->streaming;

	lockdep_assert_held(&sensor->mutex);
	if (live) {
		if (sensor->live_error)
			return sensor->live_error;
		if (sensor->pace_active)
			return -EBUSY;
		if (sensor->live_updates == U64_MAX)
			return -EOVERFLOW;
		memset(&sensor->live_pace, 0, sizeof(sensor->live_pace));
		sensor->live_pace.entered = 1;
		sensor->live_pace.kind = 4;
		sensor->live_pace.first_wait_us = OV02A10_PACE_US;
		sensor->live_pace.between_wait_us = OV02A10_PACE_US;
		sensor->live_pace.begin_ns = ktime_get_ns();
		sensor->pace_active = &sensor->live_pace;
		sensor->live_updates++;
	}
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = ov02a10_pace_bus(sensor, false, regs[i], vals[i]);
		if (ret < 0)
			break;
	}
	if (ret < 0) {
		restore = ov02a10_pace_bus(sensor, false, REG_PAGE_SWITCH, REG_ENABLE);
		(void)restore; /* Keep the original error even if recovery also fails. */
	}
	if (live) {
		if (!ret && sensor->live_pace.overflow)
			ret = -EOVERFLOW;
		sensor->live_error = ret;
		ov02a10_pace_end(sensor, ret);
	}
	/* Write completion is not optical application. libcamera schedules
	 * through its standard delayed controls; measure the sensor delay.
	 * Cold startup still waits140ms and performs unchanged strict readback.
	 */
	return ret;
}

static int ov02a10_set_vblank(struct ov02a10 *sensor, int val)
{
	u32 vts = val + sensor->cur_mode->height - OV02A10_BASE_LINES;
	const u8 regs[] = { REG_PAGE_SWITCH, OV02A10_REG_VTS_H, OV02A10_REG_VTS_L, REG_GLOBAL_EFFECTIVE };
	const u8 vals[] = { REG_ENABLE, vts >> OV02A10_VTS_SHIFT, vts, REG_ENABLE };
	unsigned int i;
	int ret = 0, restore;
	bool live = sensor->streaming;

	lockdep_assert_held(&sensor->mutex);
	if (live) {
		if (sensor->live_error)
			return sensor->live_error;
		if (sensor->pace_active)
			return -EBUSY;
		if (sensor->live_updates == U64_MAX)
			return -EOVERFLOW;
		memset(&sensor->live_pace, 0, sizeof(sensor->live_pace));
		sensor->live_pace.entered = 1;
		sensor->live_pace.kind = 5;
		sensor->live_pace.first_wait_us = OV02A10_PACE_US;
		sensor->live_pace.between_wait_us = OV02A10_PACE_US;
		sensor->live_pace.begin_ns = ktime_get_ns();
		sensor->pace_active = &sensor->live_pace;
		sensor->live_updates++;
	}
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = ov02a10_pace_bus(sensor, false, regs[i], vals[i]);
		if (ret < 0)
			break;
	}
	if (ret < 0) {
		restore = ov02a10_pace_bus(sensor, false, REG_PAGE_SWITCH, REG_ENABLE);
		(void)restore; /* Keep the original error even if recovery also fails. */
	}
	if (live) {
		if (!ret && sensor->live_pace.overflow)
			ret = -EOVERFLOW;
		sensor->live_error = ret;
		ov02a10_pace_end(sensor, ret);
	}
	/* Write completion is not optical application. libcamera schedules
	 * through its standard delayed controls; measure the sensor delay.
	 * Cold startup still waits140ms and performs unchanged strict readback.
	 */
	return ret;
}


static int ov02a10_set_test_pattern(struct ov02a10 *ov02a10, int pattern)
{
	int ret;

	ret = ov02a10_pace_bus(ov02a10, false, REG_PAGE_SWITCH, REG_ENABLE);
	if (ret < 0)
		return ret;

	ret = ov02a10_pace_bus(ov02a10, false, OV02A10_REG_TEST_PATTERN,
					pattern);
	if (ret < 0)
		return ret;

	/* Control setup must not start streaming before orientation/TX setup. */
	return ov02a10_pace_bus(ov02a10, false, REG_GLOBAL_EFFECTIVE,
					 REG_ENABLE);
}

static int ov02a10_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov02a10 *ov02a10 = container_of(ctrl->handler,
					       struct ov02a10, ctrl_handler);
	s64 max_expo;
	int ret;

	/* Layout changes require idle; standard exposure/timing remain controllable. */
	if (ov02a10->streaming && ctrl->id != V4L2_CID_EXPOSURE && ctrl->id != V4L2_CID_VBLANK)
		return -EBUSY;

	/* Propagate change of current control to all related controls */
	if (ctrl->id == V4L2_CID_VBLANK) {
		/* Update max exposure while meeting expected vblanking */
		max_expo = ov02a10->cur_mode->height + ctrl->val -
			   OV02A10_EXPOSURE_MAX_MARGIN;
		__v4l2_ctrl_modify_range(ov02a10->exposure,
					 ov02a10->exposure->minimum, max_expo,
					 ov02a10->exposure->step,
					 ov02a10->exposure->default_value);
	}

	if (ctrl->id == V4L2_CID_HFLIP)
  ov02a10->fmt.code = ov02a10_bayer_code(ov02a10);

	/* V4L2 controls values will be applied only when power is already up */
	ret = pm_runtime_get_if_in_use(ov02a10->dev);
	if (ret <= 0)
		return ret;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = ov02a10_set_exposure_gain(ov02a10);
		break;
	case V4L2_CID_VBLANK:
		ret = ov02a10_set_vblank(ov02a10, ctrl->val);
		break;
	case V4L2_CID_HFLIP: {
  u8 bits = ov02a10->hflip->val | (ov02a10->vflip->val << 1);
  ret = ov02a10_pace_bus(ov02a10, false, REG_PAGE_SWITCH, 1);
  if (!ret) ret = ov02a10_pace_bus(ov02a10, false, REG_MIRROR_FLIP_CONTROL, bits);
  if (!ret) {
   ov02a10->start_record.orientation_write_value = bits;
   ret = ov02a10_pace_bus(ov02a10, false, REG_GLOBAL_EFFECTIVE, REG_ENABLE);
  }
  break;
 }
	case V4L2_CID_TEST_PATTERN:
		ret = ov02a10_set_test_pattern(ov02a10, ctrl->val);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(ov02a10->dev);

	return ret;
}

static int ov02a10_get_selection(struct v4l2_subdev *sd,
		struct v4l2_subdev_state *state, struct v4l2_subdev_selection *sel)
{
	struct ov02a10 *sensor = to_ov02a10(sd);
	(void)state;
	if (sel->pad || sel->stream)
		return -EINVAL;
	switch (sel->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP:
		sel->r = (struct v4l2_rect){ .width = sensor->cur_mode->width,
			.height = sensor->cur_mode->height };
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_subdev_video_ops ov02a10_video_ops = {
	.s_stream = ov02a10_s_stream,
};

static const struct v4l2_subdev_pad_ops ov02a10_pad_ops = {
	.get_selection = ov02a10_get_selection,
	.enum_mbus_code = ov02a10_enum_mbus_code,
	.enum_frame_size = ov02a10_enum_frame_sizes,
	.get_fmt = ov02a10_get_fmt,
	.set_fmt = ov02a10_set_fmt,
};

static const struct v4l2_subdev_ops ov02a10_subdev_ops = {
	.video	= &ov02a10_video_ops,
	.pad	= &ov02a10_pad_ops,
};

static const struct v4l2_subdev_internal_ops ov02a10_internal_ops = {
	.init_state = ov02a10_init_state,
};

static const struct media_entity_operations ov02a10_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_ctrl_ops ov02a10_ctrl_ops = {
	.s_ctrl = ov02a10_set_ctrl,
};

static int ov02a10_initialize_controls(struct ov02a10 *ov02a10)
{
	const struct ov02a10_mode *mode;
	struct v4l2_ctrl_handler *handler;
	struct v4l2_ctrl *ctrl;
	struct v4l2_fwnode_device_properties props;
	s64 exposure_max;
	s64 vblank_def;
	s64 pixel_rate;
	int ret;

	handler = &ov02a10->ctrl_handler;
	mode = ov02a10->cur_mode;
	ret = v4l2_ctrl_handler_init(handler, 12);
	if (ret)
		return ret;

	handler->lock = &ov02a10->mutex;

	/* Fixed optical pixel pitch from the module vendor specification. */
	{
		const struct v4l2_area pixel = { .width = 1750, .height = 1750 };
		ctrl = v4l2_ctrl_new_std_compound(handler, NULL,
			V4L2_CID_UNIT_CELL_SIZE, v4l2_ctrl_ptr_create((void *)&pixel),
			v4l2_ctrl_ptr_create(NULL), v4l2_ctrl_ptr_create(NULL));
		if (ctrl)
			ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	}

	ctrl = v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ, 0, 0,
				      link_freq_menu_items);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	pixel_rate = to_pixel_rate(0);
	v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE, 0, pixel_rate, 1,
			  pixel_rate);

	ctrl = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
		OV02A10_LINE_LENGTH - mode->width,
		OV02A10_LINE_LENGTH - mode->width, 1,
		OV02A10_LINE_LENGTH - mode->width);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank_def = OV02A10_DEFAULT_VBLANK;
	ov02a10->vblank = v4l2_ctrl_new_std(handler, &ov02a10_ctrl_ops, V4L2_CID_VBLANK,
			  OV02A10_MIN_VBLANK, OV02A10_VTS_MAX - mode->height, 1,
			  vblank_def);

	exposure_max = mode->height + vblank_def - 4;
	ov02a10->exposure = v4l2_ctrl_new_std(handler, &ov02a10_ctrl_ops,
					      V4L2_CID_EXPOSURE,
					      OV02A10_EXPOSURE_MIN,
					      exposure_max,
					      OV02A10_EXPOSURE_STEP,
					      mode->exp_def);

	ov02a10->gain = v4l2_ctrl_new_std(handler, &ov02a10_ctrl_ops,
			  V4L2_CID_ANALOGUE_GAIN, OV02A10_GAIN_MIN,
			  OV02A10_GAIN_MAX, OV02A10_GAIN_STEP,
			  OV02A10_GAIN_DEFAULT);

	if (!handler->error) {
		ov02a10->exposure_gain[0] = ov02a10->exposure;
		ov02a10->exposure_gain[1] = ov02a10->gain;
		v4l2_ctrl_cluster(2, ov02a10->exposure_gain);
	}

	ov02a10->hflip = v4l2_ctrl_new_std(handler, &ov02a10_ctrl_ops,
   V4L2_CID_HFLIP, 0, 1, 1, 0);
 ov02a10->vflip = v4l2_ctrl_new_std(handler, &ov02a10_ctrl_ops,
   V4L2_CID_VFLIP, 0, 1, 1, 0);
 if (!handler->error) {
  ov02a10->flips[0] = ov02a10->hflip; ov02a10->flips[1] = ov02a10->vflip;
  v4l2_ctrl_cluster(2, ov02a10->flips);
  ov02a10->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
  ov02a10->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
 }

	ov02a10->test_pattern = v4l2_ctrl_new_std_menu_items(handler, &ov02a10_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(ov02a10_test_pattern_menu) - 1,
				     0, 0, ov02a10_test_pattern_menu);

	ret = v4l2_fwnode_device_parse(ov02a10->dev, &props);
	if (!ret)
		ret = v4l2_ctrl_new_fwnode_properties(handler, &ov02a10_ctrl_ops, &props);
	if (ret) {
		v4l2_ctrl_handler_free(handler);
		return ret;
	}

	if (handler->error) {
		ret = handler->error;
		dev_err(ov02a10->dev, "failed to init controls(%d)\n", ret);
		goto err_free_handler;
	}

	ov02a10->subdev.ctrl_handler = handler;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

static int ov02a10_check_hwcfg(struct device *dev, struct ov02a10 *ov02a10)
{
	struct fwnode_handle *ep;
	struct fwnode_handle *fwnode = dev_fwnode(dev);
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	unsigned int i, j;
	u32 clk_volt;
	int ret;

	if (!fwnode)
		return -EINVAL;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -ENXIO;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	if (ret) {
		fwnode_handle_put(ep);
		return ret;
	}

	/* Optional indication of MIPI clock voltage unit */
	ret = fwnode_property_read_u32(ep, "ovti,mipi-clock-voltage",
				       &clk_volt);
	fwnode_handle_put(ep);

	if (!ret)
		ov02a10->mipi_clock_voltage = clk_volt;

	for (i = 0; i < ARRAY_SIZE(link_freq_menu_items); i++) {
		for (j = 0; j < bus_cfg.nr_of_link_frequencies; j++) {
			if (link_freq_menu_items[i] ==
				bus_cfg.link_frequencies[j])
				break;
		}

		if (j == bus_cfg.nr_of_link_frequencies) {
			dev_err(dev, "no link frequency %lld supported\n",
				link_freq_menu_items[i]);
			ret = -EINVAL;
			break;
		}
	}

	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int ov02a10_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ov02a10 *ov02a10;
	unsigned int i;
	unsigned int rotation;
	int ret;

	ov02a10 = devm_kzalloc(dev, sizeof(*ov02a10), GFP_KERNEL);
	if (!ov02a10)
		return -ENOMEM;

	ov02a10->dev = dev;
	/* Keep the board endpoint override parsed by check_hwcfg(). */
	ov02a10->mipi_clock_voltage = OV02A10_MIPI_TX_SPEED_DEFAULT;

	ret = ov02a10_check_hwcfg(dev, ov02a10);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to check HW configuration\n");

	v4l2_i2c_subdev_init(&ov02a10->subdev, client, &ov02a10_subdev_ops);
	ov02a10->subdev.internal_ops = &ov02a10_internal_ops;

	ov02a10->fmt.code = MEDIA_BUS_FMT_SBGGR10_1X10;

	/* Optional indication of physical rotation of sensor */
	rotation = 0;
	device_property_read_u32(dev, "rotation", &rotation);
	if (rotation == 180) {
		ov02a10->upside_down = true;
	}

	ov02a10->eclk = devm_v4l2_sensor_clk_get_legacy(dev, "eclk", false, 0);
	if (IS_ERR(ov02a10->eclk))
		return dev_err_probe(dev, PTR_ERR(ov02a10->eclk),
				     "failed to get eclk\n");

	if (clk_get_rate(ov02a10->eclk) != OV02A10_ECLK_FREQ)
		dev_warn(dev, "eclk mismatched, mode is based on 24MHz\n");

	ov02a10->pd_gpio = devm_gpiod_get(dev, "powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(ov02a10->pd_gpio))
		return dev_err_probe(dev, PTR_ERR(ov02a10->pd_gpio),
				     "failed to get powerdown-gpios\n");

	ov02a10->rst_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ov02a10->rst_gpio))
		return dev_err_probe(dev, PTR_ERR(ov02a10->rst_gpio),
				     "failed to get reset-gpios\n");

	for (i = 0; i < ARRAY_SIZE(ov02a10_supply_names); i++)
		ov02a10->supplies[i].supply = ov02a10_supply_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ov02a10_supply_names),
				      ov02a10->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	mutex_init(&ov02a10->mutex);
	INIT_LIST_HEAD(&ov02a10->telemetry_entry);
	ov02a10->probe_instance_ns = ktime_get_ns();
	ov02a10_record_clear(ov02a10);

	/* Set default mode */
	ov02a10->cur_mode = &supported_modes[0];

	ret = ov02a10_initialize_controls(ov02a10);
	if (ret) {
		dev_err_probe(dev, ret, "failed to initialize controls\n");
		goto err_destroy_mutex;
	}

	/* Initialize subdev */
	ov02a10->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	ov02a10->subdev.entity.ops = &ov02a10_subdev_entity_ops;
	ov02a10->subdev.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ov02a10->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&ov02a10->subdev.entity, 1, &ov02a10->pad);
	if (ret < 0) {
		dev_err_probe(dev, ret, "failed to initialize entity pads\n");
		goto err_free_handler;
	}

	pm_runtime_enable(dev);
	if (pm_runtime_enabled(dev)) {
		/* Probe experiment: power_on() reads the ID, without streaming. */
		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0) {
			dev_err_probe(dev, ret, "failed to verify sensor ID\n");
			goto err_power_off;
		}
		pm_runtime_put_sync_suspend(dev);
	} else {
		ret = ov02a10_power_on(dev);
		if (ret < 0) {
			dev_err_probe(dev, ret, "failed to power on\n");
			goto err_clean_entity;
		}
	}

	ret = ov02a10_record_publish(ov02a10);
	if (ret)
		goto err_power_off;

	ret = v4l2_async_register_subdev(&ov02a10->subdev);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register V4L2 subdev\n");
		ov02a10_record_unpublish(ov02a10);
		goto err_power_off;
	}

	return 0;

err_power_off:
	if (pm_runtime_enabled(dev))
		pm_runtime_disable(dev);
	else
		ov02a10_power_off(dev);
err_clean_entity:
	media_entity_cleanup(&ov02a10->subdev.entity);
err_free_handler:
	v4l2_ctrl_handler_free(ov02a10->subdev.ctrl_handler);
err_destroy_mutex:
	mutex_destroy(&ov02a10->mutex);

	return ret;
}

static void ov02a10_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov02a10 *ov02a10 = to_ov02a10(sd);

	/* Unlink/drain getter first; wait for active sysfs shows with neither
	 * registry nor sensor mutex held. Only then tear down devm-owned state.
	 */
	ov02a10_record_unpublish(ov02a10);
	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(ov02a10->dev);
	if (!pm_runtime_status_suspended(ov02a10->dev))
		ov02a10_power_off(ov02a10->dev);
	pm_runtime_set_suspended(ov02a10->dev);
	mutex_destroy(&ov02a10->mutex);
}

static const struct of_device_id ov02a10_of_match[] = {
	{ .compatible = "ovti,ov02a10" },
	{}
};
MODULE_DEVICE_TABLE(of, ov02a10_of_match);

static struct i2c_driver ov02a10_i2c_driver = {
	.driver = {
		.name = "ov02a10",
		.pm = &ov02a10_pm_ops,
		.of_match_table = ov02a10_of_match,
	},
	.probe		= ov02a10_probe,
	.remove		= ov02a10_remove,
};
module_i2c_driver(ov02a10_i2c_driver);

MODULE_AUTHOR("Dongchun Zhu <dongchun.zhu@mediatek.com>");
MODULE_DESCRIPTION("OV02A10 standard clustered exposure/gain with strict startup telemetry");
MODULE_LICENSE("GPL v2");
