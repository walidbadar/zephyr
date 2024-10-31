/*
 * Copyright (c) 2024 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT elan_ekt2101

#include <math.h>
#include <zephyr/init.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include "ekt2101.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(EKT2101, CONFIG_SENSOR_LOG_LEVEL);

int ekt2101_get_touchpad_data(const struct device *dev,
			   struct ekt2101_reg_data *hw_data)
{
	int ret;
	uint8_t reg_data[EKT2101_PACKET_SIZE];
	const struct ekt2101_config *config = dev->config;

	ret = i2c_read_dt(&config->i2c, reg_data, EKT2101_PACKET_SIZE);
	if (ret != 0) {
		return ret;
	}

	hw_data->reg = ((reg_data[2] & 0x0f) << 16) + (reg_data[1] << 8) + (reg_data[0] & 0xfc);

	return 0;
}

static void ekt2101_touchpad_convert(const struct device *dev,
				  struct sensor_value *val)
{
	uint32_t button_value;
	struct ekt2101_data *drv_data = dev->data;
	button_value = drv_data->hw_data.reg;

	for(uint8_t bits=0; bits<18; bits++){
        if(button_value == (int)pow(2,bits))
			val->val1 = button_value + 1;
    }

	LOG_DBG("Button value: %d", val->val1);
}

static int ekt2101_sample_fetch(const struct device *dev,
				   enum sensor_channel chan)
{
	int ret;
	struct ekt2101_data *drv_data = dev->data;

	ret = ekt2101_get_touchpad_data(dev, &drv_data->hw_data);
	if (ret != 0) {
		LOG_ERR("Unable to get sensor data: %d", ret);
		return ret;
	}
	return 0;
}

static int ekt2101_channel_get(const struct device *dev,
				  enum sensor_channel chan,
				  struct sensor_value *val)
{
	ekt2101_touchpad_convert(dev, val);

	return 0;
}

static const struct sensor_driver_api ekt2101_driver_api = {
	.sample_fetch = ekt2101_sample_fetch,
	.channel_get = ekt2101_channel_get,
	.trigger_set = ekt2101_trigger_set,
};

static int ekt2101_init(const struct device *dev)
{
	int ret;
	const struct ekt2101_config *config = dev->config;

	if (!device_is_ready(config->i2c.bus)) {
		LOG_ERR("I2C bus device not ready");
		return -ENODEV;
	}

	ret = ekt2101_init_interrupt(dev);
	if (ret != 0) {
		LOG_ERR("Failed to initialize interrupt!");
		return -EIO;
	}

	return 0;
}

#define EKT2101_DEVICE_INIT(inst)					\
	SENSOR_DEVICE_DT_INST_DEFINE(inst,				\
			      ekt2101_init,				\
			      NULL,					\
			      &ekt2101_data_##inst,			\
			      &ekt2101_config_##inst,			\
			      POST_KERNEL,				\
			      CONFIG_SENSOR_INIT_PRIORITY,		\
			      &ekt2101_driver_api);

#define EKT2101_CONFIG(inst)					\
	{								\
		.i2c = I2C_DT_SPEC_INST_GET(inst),		\
		.interrupt = GPIO_DT_SPEC_INST_GET_OR(inst, int_gpios, { 0 }),		\
	}

#define EKT2101_DEFINE(inst)								\
	static struct ekt2101_data ekt2101_data_##inst;				\
	static const struct ekt2101_config ekt2101_config_##inst =		\
		EKT2101_CONFIG(inst);				\
	EKT2101_DEVICE_INIT(inst)

DT_INST_FOREACH_STATUS_OKAY(EKT2101_DEFINE)
