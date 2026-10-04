#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <zmk-mpr121-module01/disc_positions.h>

#if __has_include(<drivers/behavior.h>)
#include <drivers/behavior.h>
#else
#include <zmk/behavior.h>
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(DISC_MODE_TOGGLE)) || \
    DT_NODE_EXISTS(DT_NODELABEL(disc_mode_toggle))

#if DT_NODE_EXISTS(DT_NODELABEL(DISC_MODE_TOGGLE))
#define DISC_MODE_NODE DT_NODELABEL(DISC_MODE_TOGGLE)
#else
#define DISC_MODE_NODE DT_NODELABEL(disc_mode_toggle)
#endif

static int disc_mode_binding_pressed(const struct device *dev,
				     uint32_t position,
				     int64_t timestamp)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(position);
	ARG_UNUSED(timestamp);

	disc_mode_toggle_global();

	return 0;
}

static int disc_mode_binding_released(const struct device *dev,
				      uint32_t position,
				      int64_t timestamp)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(position);
	ARG_UNUSED(timestamp);

	return 0;
}

static const struct behavior_driver_api disc_mode_behavior_api = {
	.binding_pressed = disc_mode_binding_pressed,
	.binding_released = disc_mode_binding_released,
};

static int disc_mode_behavior_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

DEVICE_DT_DEFINE(DISC_MODE_NODE,
		 disc_mode_behavior_init,
		 NULL,
		 NULL,
		 NULL,
		 POST_KERNEL,
		 CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
		 &disc_mode_behavior_api);

#endif /* DT_NODE_EXISTS(...) */
