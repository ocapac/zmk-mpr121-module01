#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <stdbool.h>
#include <stdint.h>

#include <zmk-mpr121-module01/disc_positions.h>

/*
 * Try to use the real ZMK behavior headers if available.
 *
 * If they are not available, provide compatible local fallbacks so this
 * module can still compile as an external module.
 */

#if defined(__has_include)

#if __has_include(<zmk/behavior.h>)
#include <zmk/behavior.h>
#define DISC_HAS_ZMK_BEHAVIOR_BINDING 1
#endif

#if __has_include(<drivers/behavior.h>)
#include <drivers/behavior.h>
#define DISC_HAS_ZMK_BEHAVIOR_DRIVER_API 1
#endif

#endif /* __has_include */

#ifndef DISC_HAS_ZMK_BEHAVIOR_BINDING
struct zmk_behavior_binding {
	char *behavior_dev;
	uint32_t param1;
	uint32_t param2;
};

struct zmk_behavior_binding_event {
	uint32_t position;
	int64_t timestamp;
};
#endif

#ifndef DISC_HAS_ZMK_BEHAVIOR_DRIVER_API
struct behavior_driver_api {
	int (*binding_convert_central_state_dependent_params)(
		struct zmk_behavior_binding *binding,
		struct zmk_behavior_binding_event event);

	int (*binding_pressed)(
		struct zmk_behavior_binding *binding,
		struct zmk_behavior_binding_event event);

	int (*binding_released)(
		struct zmk_behavior_binding *binding,
		struct zmk_behavior_binding_event event);
};
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(disc_mode_toggle))
#define DISC_MODE_NODE DT_NODELABEL(disc_mode_toggle)
#elif DT_NODE_EXISTS(DT_NODELABEL(DISC_MODE_TOGGLE))
#define DISC_MODE_NODE DT_NODELABEL(DISC_MODE_TOGGLE)
#endif

#ifdef DISC_MODE_NODE

static int disc_mode_binding_pressed(struct zmk_behavior_binding *binding,
				     struct zmk_behavior_binding_event event)
{
	ARG_UNUSED(binding);
	ARG_UNUSED(event);

	disc_mode_toggle_global();

	return 0;
}

static int disc_mode_binding_released(struct zmk_behavior_binding *binding,
				      struct zmk_behavior_binding_event event)
{
	ARG_UNUSED(binding);
	ARG_UNUSED(event);

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

#define DISC_MODE_BEHAVIOR_INIT_PRIORITY 91

DEVICE_DT_DEFINE(DISC_MODE_NODE,
		 disc_mode_behavior_init,
		 NULL,
		 NULL,
		 NULL,
		 POST_KERNEL,
		 DISC_MODE_BEHAVIOR_INIT_PRIORITY,
		 &disc_mode_behavior_api);

#endif /* DISC_MODE_NODE */
