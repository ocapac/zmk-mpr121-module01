#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/input/input.h>

#include <zmk-mpr121-module01/disc_positions.h>

#if __has_include(<zmk/hid.h>)
#include <zmk/hid.h>
#endif

#define DISC_DEBUG_DISABLE_MPR121_SCAN 0
#define DISC_DEBUG_DISABLE_MPR121_IRQ  0
#define DISC_DEBUG_MPR121_READ_ONLY    0
#define DISC_DEBUG_DISABLE_DISC_REPORTS 0
#define DISC_DEBUG_DISABLE_HID_OUTPUT   0
#define DISC_ENABLE_PROXIMITY 0

LOG_MODULE_REGISTER(mpr121_disc, LOG_LEVEL_INF);

#define DT_DRV_COMPAT zmk_kscan_mpr121_disc

#if !DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
#warning "zmk,kscan-mpr121-disc is not enabled in devicetree"
#else


/*
 * Disc geometry.
 *
 * Electrode pitch is 4.95 mm center-to-center.
 * With four columns and four rows, nominal coordinates are:
 * -7.425 mm, -2.475 mm, +2.475 mm, +7.425 mm.
 */
static const int32_t disc_col_x_um[4] = {
	-7425, -2475, 2475, 7425
};

static const int32_t disc_row_y_um[4] = {
	-7425, -2475, 2475, 7425
};

#define DISC_WORK_INTERVAL_MS       16

#define DISC_TAP_MAX_MS             250
#define DISC_TAP_MAX_MOVE_UM        2500

#define DISC_ROTARY_INNER_UM        7000

#define DISC_VELOCITY_DIVISOR       1200
#define DISC_DECAY_PERCENT          85

#define DISC_TOUCH_DEBOUNCE_COUNT      3
#define DISC_MIN_EVENT_INTERVAL_MS     150

/*
 * MPR121 registers.
 *
 * Register names follow common MPR121 usage. Some silicon revisions may need
 * minor tuning, but this is the standard operating map.
 */
#define MPR121_REG_TOUCH_STATUS_L   0x00
#define MPR121_REG_TOUCH_STATUS_H   0x01

#define MPR121_REG_MHD_RISING       0x2B
#define MPR121_REG_NHD_RISING       0x2C
#define MPR121_REG_NCL_RISING       0x2D
#define MPR121_REG_FDL_RISING       0x2E

#define MPR121_REG_MHD_FALLING      0x2F
#define MPR121_REG_NHD_FALLING      0x30
#define MPR121_REG_NCL_FALLING      0x31
#define MPR121_REG_FDL_FALLING      0x32

#define MPR121_REG_NHD_TOUCHED      0x33
#define MPR121_REG_NCL_TOUCHED      0x34
#define MPR121_REG_FDL_TOUCHED      0x35

#define MPR121_REG_E0TTH            0x41
#define MPR121_REG_E0RTH            0x42

#define MPR121_REG_AFE1             0x5C
#define MPR121_REG_AFE2             0x5D
#define MPR121_REG_ELECTRODE_CONF   0x5E

#define MPR121_REG_SOFT_RESET       0x80

#define MPR121_SPS_MASK             0x07
#define MPR121_SPS_1MS              0x00
#define MPR121_SPS_16MS             0x04
#define MPR121_SPS_128MS            0x07

#define DISC_ZONE_TOP               0
#define DISC_ZONE_BOTTOM            1
#define DISC_ZONE_LEFT              2
#define DISC_ZONE_RIGHT             3

struct disc_cfg {
	uint16_t addr;
	struct gpio_dt_spec irq;

	uint8_t touch_threshold;
	uint8_t prox_threshold;

	int32_t center_dead_radius_um;
	int32_t swipe_threshold_um;
	int32_t rotary_threshold_deg;
	int32_t idle_timeout_ms;
};

struct disc_data {
	const struct device *dev;
	const struct device *bus;
	uint16_t addr;

	kscan_callback_t callback;

	struct k_work_delayable work;
	struct gpio_callback irq_cb;

	bool enabled;

	bool disc_raw_active;
	uint8_t disc_debounce_count;
	bool disc_active;
	int64_t last_disc_event_ms;

	bool prox_active;
	bool prox_enter_reported;
	bool prox_idle_reported;
	bool sample_slow;
	int64_t no_prox_since_ms;

	bool touch_down;
	int64_t touch_start_ms;

	int32_t start_x_um;
	int32_t start_y_um;
	int32_t last_x_um;
	int32_t last_y_um;
	int32_t dist_center_um;

	bool center_armed;
	bool gesture_sent;

	bool rotary_active;
	int start_zone;
	int32_t last_angle_deg;
	int32_t accum_angle_deg;

	int32_t vel_x_um;
	int32_t vel_y_um;
};

static struct disc_data disc_data;

static atomic_t disc_mode = ATOMIC_INIT(DISC_MODE_CURSOR);

void disc_mode_toggle_global(void)
{
	uint8_t current = (uint8_t)atomic_get(&disc_mode);

	atomic_set(&disc_mode,
		   current == DISC_MODE_CURSOR ? DISC_MODE_SCROLL : DISC_MODE_CURSOR);

	LOG_INF("Disc mode is now %s",
		disc_mode_get_global() == DISC_MODE_CURSOR ? "cursor" : "scroll");
}

uint8_t disc_mode_get_global(void)
{
	return (uint8_t)atomic_get(&disc_mode);
}

static const struct disc_cfg disc_cfg = {
	.addr = DT_INST_REG_ADDR(0),

	.irq = GPIO_DT_SPEC_GET(DT_DRV_INST(0), irq_gpios),

	.touch_threshold = DT_INST_PROP(0, touch_threshold),
	.prox_threshold = DT_INST_PROP(0, prox_threshold),

	.center_dead_radius_um = DT_INST_PROP(0, center_dead_radius_mm) * 1000,
	.swipe_threshold_um = DT_INST_PROP(0, swipe_threshold_mm) * 1000,
	.rotary_threshold_deg = DT_INST_PROP(0, rotary_threshold_deg),
	.idle_timeout_ms = DT_INST_PROP(0, idle_timeout_ms),
};

static int mpr_write(uint8_t reg, uint8_t val)
{
	if (disc_data.bus == NULL) {
		return -ENODEV;
	}

	return i2c_burst_write(disc_data.bus, disc_data.addr, reg, &val, 1);
}

static int mpr_read(uint8_t reg, uint8_t *val)
{
	if (disc_data.bus == NULL) {
		return -ENODEV;
	}

	return i2c_burst_read(disc_data.bus, disc_data.addr, reg, val, 1);
}

static int mpr_burst_read(uint8_t reg, uint8_t *buf, uint32_t len)
{
	if (disc_data.bus == NULL) {
		return -ENODEV;
	}

	return i2c_burst_read(disc_data.bus, disc_data.addr, reg, buf, len);
}

static void disc_report_position(uint32_t position, bool pressed)
{
#if DISC_DEBUG_DISABLE_DISC_REPORTS
	if (position >= DISC_POS_TAP_CENTER) {
		return;
	}
#endif

	if (disc_data.callback == NULL) {
		return;
	}

	disc_data.callback(disc_data.dev, 0, position, pressed);
}

static void disc_pulse_position(uint32_t position)
{
	int64_t now = k_uptime_get();

	if (disc_data.last_disc_event_ms != 0 &&
	    (now - disc_data.last_disc_event_ms) < DISC_MIN_EVENT_INTERVAL_MS) {
		return;
	}

	disc_data.last_disc_event_ms = now;

	disc_report_position(position, true);
	disc_report_position(position, false);
}

static int32_t disc_abs(int32_t value)
{
	return value < 0 ? -value : value;
}

static int32_t disc_clamp(int32_t value, int32_t min, int32_t max)
{
	if (value < min) {
		return min;
	}

	if (value > max) {
		return max;
	}

	return value;
}

static int32_t disc_distance_um(int32_t x, int32_t y)
{
	int32_t ax = disc_abs(x);
	int32_t ay = disc_abs(y);

	return MAX(ax, ay) + (MIN(ax, ay) / 2);
}

static int32_t disc_angle_deg(int32_t x, int32_t y)
{
	int32_t ax = disc_abs(x);
	int32_t ay = disc_abs(y);
	int32_t angle;

	if (ax == 0 && ay == 0) {
		return 0;
	}

	/* atan2 approximation covering all 4 quadrants */
	if (ax > ay) {
		angle = (45 * ay) / ax;
	} else {
		angle = 90 - (45 * ax) / ay;
	}

	if (x >= 0 && y <= 0) {
		return -angle;
	} else if (x < 0 && y <= 0) {
		return -180 + angle;
	} else if (x < 0 && y > 0) {
		return 180 - angle;
	} else {
		return angle;
	}
}

static int32_t disc_angle_diff(int32_t new_angle, int32_t old_angle)
{
	int32_t diff = new_angle - old_angle;

	if (diff > 180) {
		diff -= 360;
	}

	if (diff < -180) {
		diff += 360;
	}

	return diff;
}

static int disc_zone_from_angle(int32_t angle)
{
	/* Top: -45 to -135 degrees */
	if (angle <= -45 && angle >= -135) {
		return DISC_ZONE_TOP;
	}

	/* Bottom: 45 to 135 degrees */
	if (angle >= 45 && angle <= 135) {
		return DISC_ZONE_BOTTOM;
	}

	/* Right: -45 to 45 degrees */
	if (angle > -45 && angle < 45) {
		return DISC_ZONE_RIGHT;
	}

	/* Left: > 135 or < -135 degrees */
	return DISC_ZONE_LEFT;
}

static bool disc_position_from_state(uint16_t state, int32_t *x, int32_t *y)
{
	int col_count = 0;
	int row_count = 0;
	int32_t sum_x = 0;
	int32_t sum_y = 0;

	for (int i = 0; i < 4; i++) {
		if (state & BIT(i)) {
			sum_x += disc_col_x_um[i];
			col_count++;
		}
	}

	for (int i = 0; i < 4; i++) {
		if (state & BIT(4 + i)) {
			sum_y += disc_row_y_um[i];
			row_count++;
		}
	}

	if (col_count == 0 && row_count == 0) {
		return false;
	}

	if (col_count > 0) {
		*x = sum_x / col_count;
	} else {
		*x = disc_data.touch_down ? disc_data.last_x_um : 0;
	}

	if (row_count > 0) {
		*y = sum_y / row_count;
	} else {
		*y = disc_data.touch_down ? disc_data.last_y_um : 0;
	}

	return true;
}

static uint32_t disc_tap_position(int32_t x, int32_t y)
{
	if (disc_abs(x) <= disc_cfg.center_dead_radius_um &&
	    disc_abs(y) <= disc_cfg.center_dead_radius_um) {
		return DISC_POS_TAP_CENTER;
	}

	if (disc_abs(y) > disc_abs(x)) {
		return y < 0 ? DISC_POS_TAP_TOP : DISC_POS_TAP_BOTTOM;
	}

	return x < 0 ? DISC_POS_TAP_LEFT : DISC_POS_TAP_RIGHT;
}

static uint32_t disc_swipe_position(int32_t dx, int32_t dy)
{
	if (disc_abs(dx) > disc_abs(dy)) {
		return dx > 0 ? DISC_POS_SWIPE_RIGHT : DISC_POS_SWIPE_LEFT;
	}

	return dy > 0 ? DISC_POS_SWIPE_DOWN : DISC_POS_SWIPE_UP;
}

static uint32_t disc_rotary_position(int zone, bool clockwise)
{
	static const uint32_t rot_positions[4][2] = {
		[DISC_ZONE_TOP] = {
			DISC_POS_ROT_TOP_CW,
			DISC_POS_ROT_TOP_CCW,
		},
		[DISC_ZONE_BOTTOM] = {
			DISC_POS_ROT_BOTTOM_CW,
			DISC_POS_ROT_BOTTOM_CCW,
		},
		[DISC_ZONE_LEFT] = {
			DISC_POS_ROT_LEFT_CW,
			DISC_POS_ROT_LEFT_CCW,
		},
		[DISC_ZONE_RIGHT] = {
			DISC_POS_ROT_RIGHT_CW,
			DISC_POS_ROT_RIGHT_CCW,
		},
	};

	if (zone < 0 || zone > 3) {
		zone = DISC_ZONE_TOP;
	}

	return rot_positions[zone][clockwise ? 0 : 1];
}

static int8_t disc_to_hid_count(int32_t um)
{
	int32_t value;

	if (um == 0) {
		return 0;
	}

	value = um / DISC_VELOCITY_DIVISOR;

	if (value == 0) {
		value = um > 0 ? 1 : -1;
	}

	return (int8_t)disc_clamp(value, -8, 8);
}

static int mpr121_set_sample_period(uint8_t sps)
{
	uint8_t afe2;
	uint8_t electrode_conf;
	int err;

	err = mpr_read(MPR121_REG_ELECTRODE_CONF, &electrode_conf);
	if (err) {
		return err;
	}

	err = mpr_write(MPR121_REG_ELECTRODE_CONF, 0x00);
	if (err) {
		return err;
	}

	err = mpr_read(MPR121_REG_AFE2, &afe2);
	if (err) {
		goto restore;
	}

	afe2 &= ~MPR121_SPS_MASK;
	afe2 |= (sps & MPR121_SPS_MASK);

	err = mpr_write(MPR121_REG_AFE2, afe2);

restore:
	mpr_write(MPR121_REG_ELECTRODE_CONF, electrode_conf);

	return err;
}

static int mpr121_init(void)
{
	int err;

	err = mpr_write(MPR121_REG_SOFT_RESET, 0x63);
	if (err) {
		LOG_ERR("MPR121 soft reset failed: %d", err);
		return err;
	}

	k_msleep(10);

	/*
	 * Filtering configuration. These values are a reasonable starting
	 * point for small capacitive electrodes.
	 */
	mpr_write(MPR121_REG_MHD_RISING, 0x01);
	mpr_write(MPR121_REG_NHD_RISING, 0x01);
	mpr_write(MPR121_REG_NCL_RISING, 0x00);
	mpr_write(MPR121_REG_FDL_RISING, 0x00);

	mpr_write(MPR121_REG_MHD_FALLING, 0x01);
	mpr_write(MPR121_REG_NHD_FALLING, 0x01);
	mpr_write(MPR121_REG_NCL_FALLING, 0xFF);
	mpr_write(MPR121_REG_FDL_FALLING, 0x02);

	mpr_write(MPR121_REG_NHD_TOUCHED, 0x01);
	mpr_write(MPR121_REG_NCL_TOUCHED, 0x00);
	mpr_write(MPR121_REG_FDL_TOUCHED, 0x00);

	for (int i = 0; i < 8; i++) {
		mpr_write(MPR121_REG_E0TTH + (2 * i), disc_cfg.touch_threshold);
		mpr_write(MPR121_REG_E0RTH + (2 * i), disc_cfg.prox_threshold);
	}

	/*
	 * AFE settings:
	 * - 0x8F: reasonable charge/current baseline filter settings.
	 * - 0x21: default sample interval, SPS bits cleared.
	 */
	mpr_write(MPR121_REG_AFE1, 0x8F);
	mpr_write(MPR121_REG_AFE2, 0x21);

	/*
	 * 0x88:
	 * - enable baseline tracking
	 * - enable electrodes 0 through 7
	 */
	mpr_write(MPR121_REG_ELECTRODE_CONF, 0x88);

	mpr121_set_sample_period(MPR121_SPS_16MS);

	LOG_INF("MPR121 initialized at I2C address 0x%02x", disc_data.addr);

	return 0;
}

static void disc_start_touch(int32_t x, int32_t y, int64_t now)
{
	int32_t dist = disc_distance_um(x, y);
	int32_t angle = disc_angle_deg(x, y);

	disc_data.touch_down = true;
	disc_data.touch_start_ms = now;

	disc_data.start_x_um = x;
	disc_data.start_y_um = y;

	disc_data.last_x_um = x;
	disc_data.last_y_um = y;

	disc_data.dist_center_um = dist;

	/* 
	 * DEBUG TEST: Force center_armed to true immediately.
	 * Once we confirm cursor movement works, we will change this back 
	 * to: disc_data.center_armed = dist <= disc_cfg.center_dead_radius_um;
	 */
	disc_data.center_armed = true; 
	
	disc_data.gesture_sent = false;

	disc_data.rotary_active = dist >= DISC_ROTARY_INNER_UM;
	disc_data.start_zone = disc_zone_from_angle(angle);

	disc_data.last_angle_deg = angle;
	disc_data.accum_angle_deg = 0;

	disc_data.vel_x_um = 0;
	disc_data.vel_y_um = 0;
	
	LOG_INF("Touch Start: x=%d y=%d dist=%d armed=%d", x, y, dist, disc_data.center_armed);
}

static void disc_update_touch(int32_t x, int32_t y, int64_t now)
{
	int32_t dist = disc_distance_um(x, y);
	int32_t dx = x - disc_data.start_x_um;
	int32_t dy = y - disc_data.start_y_um;

	disc_data.last_x_um = x;
	disc_data.last_y_um = y;
	disc_data.dist_center_um = dist;

	if (dist <= disc_cfg.center_dead_radius_um) {
		disc_data.center_armed = true;
	}

	if (!disc_data.gesture_sent) {
		if (disc_data.rotary_active && dist >= DISC_ROTARY_INNER_UM) {
			int32_t angle = disc_angle_deg(x, y);
			int32_t delta = disc_angle_diff(angle, disc_data.last_angle_deg);

			disc_data.last_angle_deg = angle;
			disc_data.accum_angle_deg += delta;

			if (disc_abs(disc_data.accum_angle_deg) >= disc_cfg.rotary_threshold_deg) {
				bool clockwise = disc_data.accum_angle_deg > 0;

				disc_pulse_position(disc_rotary_position(disc_data.start_zone, clockwise));
				
				/* Endless rotary: wrap accumulator instead of stopping */
				if (disc_data.accum_angle_deg > 0) {
					disc_data.accum_angle_deg -= disc_cfg.rotary_threshold_deg;
				} else {
					disc_data.accum_angle_deg += disc_cfg.rotary_threshold_deg;
				}
			}
		} else if (!disc_data.rotary_active &&
			   MAX(disc_abs(dx), disc_abs(dy)) >= disc_cfg.swipe_threshold_um &&
			   (now - disc_data.touch_start_ms) <= 150) { /* 150ms max swipe duration */
			disc_pulse_position(disc_swipe_position(dx, dy));
			disc_data.gesture_sent = true;
		}
	}

	/* Only track cursor velocity if no gesture was sent */
	if (disc_data.center_armed && 
	    dist > disc_cfg.center_dead_radius_um && 
	    !disc_data.gesture_sent) {
		disc_data.vel_x_um = x;
		disc_data.vel_y_um = y;
	} else {
		disc_data.vel_x_um = 0;
		disc_data.vel_y_um = 0;
	}
}

static void disc_end_touch(int64_t now)
{
	int32_t dx = disc_data.last_x_um - disc_data.start_x_um;
	int32_t dy = disc_data.last_y_um - disc_data.start_y_um;
	int32_t move = MAX(disc_abs(dx), disc_abs(dy));
	int64_t duration = now - disc_data.touch_start_ms;

	if (!disc_data.gesture_sent &&
	    duration <= DISC_TAP_MAX_MS &&
	    move <= DISC_TAP_MAX_MOVE_UM) {
		disc_pulse_position(disc_tap_position(disc_data.start_x_um,
						      disc_data.start_y_um));
	}

	disc_data.touch_down = false;
	disc_data.dist_center_um = 0;

	/*
	 * Velocity is intentionally not zeroed here. It decays in
	 * disc_update_velocity() while no finger is present.
	 */
}

static void disc_update_velocity(void)
{
#if DISC_DEBUG_DISABLE_HID_OUTPUT
	return;
#endif

	int8_t hid_x;
	int8_t hid_y;

	if (disc_data.touch_down &&
	    disc_data.center_armed &&
	    disc_data.dist_center_um > disc_cfg.center_dead_radius_um &&
	    !disc_data.gesture_sent) {
		/* Stop cursor if a gesture was triggered */
		/* Keep current target velocity */
	} else {
		disc_data.vel_x_um = (disc_data.vel_x_um * DISC_DECAY_PERCENT) / 100;
		disc_data.vel_y_um = (disc_data.vel_y_um * DISC_DECAY_PERCENT) / 100;

		if (disc_abs(disc_data.vel_x_um) < (DISC_VELOCITY_DIVISOR / 2)) {
			disc_data.vel_x_um = 0;
		}

		if (disc_abs(disc_data.vel_y_um) < (DISC_VELOCITY_DIVISOR / 2)) {
			disc_data.vel_y_um = 0;
		}
	}

	hid_x = disc_to_hid_count(disc_data.vel_x_um);
	hid_y = disc_to_hid_count(disc_data.vel_y_um);

	/* DEBUG LOG: Print whenever we are trying to move the cursor */
	if (hid_x != 0 || hid_y != 0) {
		LOG_INF("Cursor Move: vel_x=%d vel_y=%d -> hid_x=%d hid_y=%d (armed=%d, gesture=%d)", 
		        disc_data.vel_x_um, disc_data.vel_y_um, hid_x, hid_y, 
		        disc_data.center_armed, disc_data.gesture_sent);
	}
	
	if (hid_x == 0 && hid_y == 0) {
		return;
	}

	/* 
	 * Use the modern Zephyr Input API. 
	 * This integrates natively with ZMK's USB/BLE stacks and input listeners.
	 */
	if (disc_mode_get_global() == DISC_MODE_CURSOR) {
		input_report_rel(disc_data.dev, INPUT_REL_X, hid_x, false, K_FOREVER);
		input_report_rel(disc_data.dev, INPUT_REL_Y, hid_y, true, K_FOREVER);
	} else {
		/* Scroll mode */
		input_report_rel(disc_data.dev, INPUT_REL_WHEEL, hid_y, true, K_FOREVER);
	}
}

static bool disc_position_from_capacitance(uint16_t state, int32_t *x, int32_t *y)
{
	uint8_t elec_data[16]; // 8 electrodes × 2 bytes each
	int err;

	// Read electrode filtered data registers (0x04-0x13)
	err = mpr_burst_read(0x04, elec_data, sizeof(elec_data));
	if (err) {
		LOG_DBG("Failed to read capacitance data: %d", err);
		return false;
	}

	// Calculate weighted centroid from capacitance values
	int32_t sum_x = 0;
	int32_t sum_y = 0;
	int32_t total_weight_x = 0;
	int32_t total_weight_y = 0;

	// Columns (electrodes 0-3)
	for (int i = 0; i < 4; i++) {
		uint16_t raw = (elec_data[i * 2] << 2) | (elec_data[i * 2 + 1] >> 6);
		int32_t weight = raw;

		if (weight > 100) { // Only use significant touches
			sum_x += disc_col_x_um[i] * weight;
			total_weight_x += weight;
		}
	}

	// Rows (electrodes 4-7)
	for (int i = 0; i < 4; i++) {
		uint16_t raw = (elec_data[(i + 4) * 2] << 2) | (elec_data[(i + 4) * 2 + 1] >> 6);
		int32_t weight = raw;

		if (weight > 100) { // Only use significant touches
			sum_y += disc_row_y_um[i] * weight;
			total_weight_y += weight;
		}
	}

	// Log for debugging
	if (total_weight_x > 0 || total_weight_y > 0) {
		LOG_DBG("Cap data: wx=%d wy=%d", total_weight_x, total_weight_y);
	}

	// Fall back to binary if capacitance data is insufficient
	if (total_weight_x == 0 && total_weight_y == 0) {
		return disc_position_from_state(state, x, y);
	}

	if (total_weight_x > 0) {
		*x = sum_x / total_weight_x;
	} else {
		*x = disc_data.touch_down ? disc_data.last_x_um : 0;
	}

	if (total_weight_y > 0) {
		*y = sum_y / total_weight_y;
	} else {
		*y = disc_data.touch_down ? disc_data.last_y_um : 0;
	}

	return true;
}

static void disc_scan_mpr121(void)
{
	uint8_t status[2];
	uint16_t state;
	int32_t x = 0;
	int32_t y = 0;
	bool has_position;
	bool raw_active;
	bool active;
	int64_t now = k_uptime_get();
	int err;

	err = mpr_burst_read(MPR121_REG_TOUCH_STATUS_L, status, sizeof(status));
	if (err) {
		return;
	}

	state = ((uint16_t)status[1] << 8) | status[0];
	state &= 0x00FF;

	// Try capacitance-based first, fall back to binary
	has_position = disc_position_from_capacitance(state, &x, &y);

#if DISC_ENABLE_PROXIMITY
	raw_active = (state != 0);
#else
	raw_active = has_position;
#endif

	if (raw_active != disc_data.disc_raw_active) {
		disc_data.disc_raw_active = raw_active;
		disc_data.disc_debounce_count = 0;
	} else if (disc_data.disc_debounce_count < DISC_TOUCH_DEBOUNCE_COUNT) {
		disc_data.disc_debounce_count++;
	}

	if (disc_data.disc_debounce_count >= DISC_TOUCH_DEBOUNCE_COUNT) {
		disc_data.disc_active = raw_active;
	}

	active = disc_data.disc_active;

#if DISC_ENABLE_PROXIMITY
	if (active && !disc_data.prox_active) {
		if (disc_data.prox_idle_reported) {
			disc_report_position(DISC_POS_PROX_IDLE, false);
			disc_data.prox_idle_reported = false;
		}

		disc_data.prox_enter_reported = true;
		disc_report_position(DISC_POS_PROX_ENTER, true);

		disc_data.no_prox_since_ms = 0;
	} else if (!active && disc_data.prox_active) {
		if (disc_data.prox_enter_reported) {
			disc_report_position(DISC_POS_PROX_ENTER, false);
			disc_data.prox_enter_reported = false;
		}

		disc_pulse_position(DISC_POS_PROX_EXIT);

		if (disc_data.touch_down) {
			disc_end_touch(now);
		}

		disc_data.no_prox_since_ms = now;
	}

	if (!active && disc_data.no_prox_since_ms != 0) {
		int64_t idle_ms = now - disc_data.no_prox_since_ms;

		if (!disc_data.prox_idle_reported && idle_ms >= disc_cfg.idle_timeout_ms) {
			disc_report_position(DISC_POS_PROX_IDLE, true);
			disc_data.prox_idle_reported = true;
		}
	}

	disc_data.prox_active = active;
#endif

	if (!active) {
		if (disc_data.touch_down) {
			disc_end_touch(now);
		}

		return;
	}

	if (has_position) {
		if (!disc_data.touch_down) {
			disc_start_touch(x, y, now);
		} else {
			disc_update_touch(x, y, now);
		}
	} else if (disc_data.touch_down) {
		disc_end_touch(now);
	}
}

#if DISC_DEBUG_MPR121_READ_ONLY
static void disc_scan_mpr121_read_only(void)
{
	static uint8_t err_count;
	static bool disabled;

	uint8_t status[2];
	int err;

	if (disabled) {
		return;
	}

	err = mpr_burst_read(MPR121_REG_TOUCH_STATUS_L, status, sizeof(status));
	if (err) {
		err_count++;

		if (err_count >= 20) {
			disabled = true;
			LOG_ERR("MPR121 read-only scan disabled after repeated I2C errors");
		} else if ((err_count % 5) == 0 && disc_data.bus != NULL) {
			i2c_recover_bus(disc_data.bus);
		}

		return;
	}

	err_count = 0;
}
#endif

static void disc_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct disc_data *data = CONTAINER_OF(dwork, struct disc_data, work);

	if (!data->enabled) {
		return;
	}

#if !DISC_DEBUG_DISABLE_MPR121_SCAN
	
#if DISC_DEBUG_MPR121_READ_ONLY
	disc_scan_mpr121_read_only();
#else
	disc_scan_mpr121();
	disc_update_velocity();
#endif
	
#endif

	k_work_reschedule(&data->work, K_MSEC(DISC_WORK_INTERVAL_MS));
}

static void disc_irq_callback(const struct device *gpio_dev,
			      struct gpio_callback *cb,
			      uint32_t pins)
{
	ARG_UNUSED(gpio_dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_work_reschedule(&disc_data.work, K_NO_WAIT);
}

static int disc_kscan_config(const struct device *dev, kscan_callback_t callback)
{
	ARG_UNUSED(dev);

	disc_data.callback = callback;

	return 0;
}

static int disc_kscan_enable_callback(const struct device *dev)
{
	ARG_UNUSED(dev);

	disc_data.enabled = true;
	k_work_reschedule(&disc_data.work, K_NO_WAIT);

	return 0;
}

static int disc_kscan_disable_callback(const struct device *dev)
{
	ARG_UNUSED(dev);

	disc_data.enabled = false;
	k_work_cancel_delayable(&disc_data.work);

	return 0;
}

static const struct kscan_driver_api disc_kscan_api = {
	.config = disc_kscan_config,
	.enable_callback = disc_kscan_enable_callback,
	.disable_callback = disc_kscan_disable_callback,
};

static int disc_kscan_init(const struct device *dev)
{
	int err;

	disc_data.dev = dev;
	disc_data.bus = DEVICE_DT_GET(DT_INST_BUS(0));
	disc_data.addr = disc_cfg.addr;

	k_work_init_delayable(&disc_data.work, disc_work_handler);

	if (!gpio_is_ready_dt(&disc_cfg.irq)) {
		LOG_ERR("MPR121 IRQ GPIO not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&disc_cfg.irq, GPIO_INPUT);
	if (err) {
		LOG_ERR("Unable to configure MPR121 IRQ GPIO: %d", err);
		return err;
	}

#if !DISC_DEBUG_DISABLE_MPR121_IRQ
	err = gpio_pin_interrupt_configure_dt(&disc_cfg.irq, GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		LOG_ERR("Unable to configure MPR121 IRQ interrupt: %d", err);
		return err;
	}

	gpio_init_callback(&disc_data.irq_cb,
			   disc_irq_callback,
			   BIT(disc_cfg.irq.pin));

	err = gpio_add_callback(disc_cfg.irq.port, &disc_data.irq_cb);
	if (err) {
		LOG_ERR("Unable to add MPR121 IRQ callback: %d", err);
		return err;
	}
#endif

	err = mpr121_init();
	if (err) {
		LOG_WRN("MPR121 initialization failed; continuing without capacitive Disc");
	}

	LOG_INF("MPR121 Disc input listener initialized");

	return 0;
}

#define DISC_KSCAN_INIT_PRIORITY 90

DEVICE_DT_INST_DEFINE(0,
		      disc_kscan_init,
		      NULL,
		      &disc_data,
		      &disc_cfg,
		      POST_KERNEL,
		      DISC_KSCAN_INIT_PRIORITY,
		      &disc_kscan_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
