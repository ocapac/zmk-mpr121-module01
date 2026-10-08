#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>

#define DT_DRV_COMPAT zmk_virtual_input

static int virtual_input_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

#define VIRTUAL_INPUT_DEFINE(inst) \
	DEVICE_DT_INST_DEFINE(inst, virtual_input_init, NULL, NULL, NULL, \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, NULL);

DT_INST_FOREACH_STATUS_OKAY(VIRTUAL_INPUT_DEFINE)
