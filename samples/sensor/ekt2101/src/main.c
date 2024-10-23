/*
 * Copyright (c) 2024, Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

const struct device *const touchpad = DEVICE_DT_GET_ONE(elan_ekt2101);

static void trigger_handler(const struct device *dev,
			    const struct sensor_trigger *trigger)
{
		int ret = sensor_sample_fetch(touchpad);
		if (ret < 0 && ret != -EBADMSG)
			printk("Sensor sample update error\n");
		
}

static int32_t read_sensor(const struct device *sensor)
{
	struct sensor_value button_val;
	int32_t ret = 0;

	struct sensor_trigger trig = {
		.type = SENSOR_TRIG_THRESHOLD,
	};

	ret = sensor_trigger_set(sensor, &trig, trigger_handler);
	if (ret != 0) {
		printf("Could not set trigger\n");
		return ret;
	}

	while (1) {
		printk("Touchpad data:\n");

		ret = sensor_channel_get(sensor, 0, &button_val);
		if (ret < 0) {
			printk("Cannot read sensor channels\n");
			return ret;
		}

		k_msleep(1000);
	}	
}

int main(void)
{
	if (!device_is_ready(touchpad)) {
		printk("sensor: device not ready.\n");
		return -ENODEV;
	}

	printf("device is %p, name is %s\n", touchpad, touchpad->name);

	read_sensor(touchpad);
	return 0;
}
