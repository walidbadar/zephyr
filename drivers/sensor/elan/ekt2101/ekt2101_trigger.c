/*
 * Copyright (c) 2024 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT elan_ekt2101

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include "ekt2101.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(EKT2101, CONFIG_SENSOR_LOG_LEVEL);

uint8_t* struct_to_bytes(struct ekt2101_reg_data *hw_data) {
    static uint8_t write_buf[EKT2101_PACKET_SIZE];
    write_buf[0] = hw_data->pid;
    write_buf[1] = hw_data->reg << 4;
    write_buf[2] = 0;
    write_buf[3] = hw_data->reserve;

    return write_buf;
}

static void setup_int(const struct device *dev,
		      bool enable)
{
	const struct ekt2101_config *config = dev->config;
	gpio_flags_t flags = enable
		? GPIO_INT_EDGE_TO_ACTIVE
		: GPIO_INT_DISABLE;

	gpio_pin_interrupt_configure_dt(&config->interrupt, flags);
}

static void handle_int(const struct device *dev)
{
	struct ekt2101_data *drv_data = dev->data;

	setup_int(dev, false);

#if defined(CONFIG_EKT2101_TRIGGER_OWN_THREAD)
	k_sem_give(&drv_data->gpio_sem);
#elif defined(CONFIG_EKT2101_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&drv_data->work);
#endif
}

static void process_int(const struct device *dev)
{
	struct ekt2101_data *drv_data = dev->data;
	const struct ekt2101_config *config = dev->config;

	if (drv_data->th_handler != NULL) {
		drv_data->th_handler(dev, drv_data->th_trigger);
	}

	setup_int(dev, true);

	/* Check for pin that asserted while we were offline */
	int pv = gpio_pin_get_dt(&config->interrupt);

	if (pv > 0) {
		handle_int(dev);
	}
}

static void ekt2101_gpio_callback(const struct device *dev,
				  struct gpio_callback *cb, uint32_t pins)
{
	struct ekt2101_data *drv_data =
		CONTAINER_OF(cb, struct ekt2101_data, gpio_cb);

	handle_int(drv_data->dev);
}

#if defined(CONFIG_EKT2101_TRIGGER_OWN_THREAD)
static void ekt2101_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct ekt2101_data *drv_data = p1;

	while (true) {
		k_sem_take(&drv_data->gpio_sem, K_FOREVER);
		process_int(drv_data->dev);
	}
}

#elif defined(CONFIG_EKT2101_TRIGGER_GLOBAL_THREAD)
static void ekt2101_work_cb(struct k_work *work)
{
	struct ekt2101_data *drv_data =
		CONTAINER_OF(work, struct ekt2101_data, work);

	process_int(drv_data->dev);
}
#endif

int ekt2101_trigger_set(const struct device *dev,
			const struct sensor_trigger *trig,
			sensor_trigger_handler_t handler)
{
	struct ekt2101_data *drv_data = dev->data;
	const struct ekt2101_config *config = dev->config;

	if (!config->interrupt.port) {
		return -ENOTSUP;
	}

	setup_int(dev, false);

	if (trig->type != SENSOR_TRIG_THRESHOLD) {
		LOG_ERR("Unsupported sensor trigger");
		return -ENOTSUP;
	}
	drv_data->th_handler = handler;

	if (handler != NULL) {
		drv_data->th_trigger = trig;

		setup_int(dev, true);

		/* Check whether already asserted */
		int pv = gpio_pin_get_dt(&config->interrupt);

		if (pv > 0) {
			handle_int(dev);
		}
	}

	return 0;
}

// int ekt2101_trigger_set(const struct device *dev,
// 			const struct sensor_trigger *trig,
// 			sensor_trigger_handler_t handler)
// {
// 	int ret;
// 	const struct ekt2101_config *config = dev->config;
// 	struct ekt2101_data *drv_data = dev->data;
// 	uint8_t *write_buf;

// 	ret = gpio_pin_interrupt_configure_dt(&config->interrupt,
// 					      GPIO_INT_DISABLE);
// 	if (ret != 0) {
// 		LOG_ERR("Unable to configure interrupt to disable: %d", ret);
// 		return ret;
// 	}

// 	if(trig->type == SENSOR_TRIG_TAP) {
// 		drv_data->th_handler = handler;
// 		drv_data->th_trigger = trig;
// 		drv_data->hw_data.pid = EKT2101_PID_3;
// 		drv_data->hw_data.reg = EKT2101_READ_BUTTON_STATUS;
// 		drv_data->hw_data.reserve = EKT2101_READ_BUTTON_STATUS;
		
// 		write_buf = struct_to_bytes(&drv_data->hw_data);
// 		ret = i2c_write_dt(&config->i2c, write_buf, EKT2101_PACKET_SIZE);
// 		if (ret != 0) {
// 			LOG_ERR("Unable to write sensor data: %d", ret);;
// 			return ret;
// 		}
// 		k_usleep(50);

// 	} else if(trig->type == SENSOR_TRIG_DATA_READY){
// 		drv_data->th_handler = handler;
// 		drv_data->th_trigger = trig;
// 	} else {
// 		LOG_ERR("Unsupported sensor trigger: %d", ret);
// 		return -ENOTSUP;
// 	}

// 	ret = ekt2101_get_touchpad_data(dev, &drv_data->hw_data);
// 	if (ret != 0) {
// 		LOG_ERR("Unable to get sensor data: %d", ret);
// 		return ret;
// 	}

// 	ret = gpio_pin_interrupt_configure_dt(&config->interrupt,
// 					      GPIO_INT_EDGE_TO_INACTIVE);
// 	if (ret != 0) {
// 		LOG_ERR("Unable to configure interrupt level: %d", ret);
// 		return ret;
// 	}

// 	return 0;
// }

int ekt2101_init_interrupt(const struct device *dev)
{
	struct ekt2101_data *drv_data = dev->data;
	const struct ekt2101_config *config = dev->config;
	int ret;

	if (!gpio_is_ready_dt(&config->interrupt)) {
		LOG_ERR("GPIO port %s not ready", config->interrupt.port->name);
		return -EINVAL;
	}

	ret = gpio_pin_configure_dt(&config->interrupt, GPIO_INPUT);
	if (ret != 0) {
		return ret;
	}

	gpio_init_callback(&drv_data->gpio_cb,
			   ekt2101_gpio_callback,
			   BIT(config->interrupt.pin));

	ret = gpio_add_callback(config->interrupt.port, &drv_data->gpio_cb);
	if (ret != 0) {
		LOG_ERR("Failed to set gpio callback!");
		return ret;
	}

	drv_data->dev = dev;

#if defined(CONFIG_EKT2101_TRIGGER_OWN_THREAD)
	k_sem_init(&drv_data->gpio_sem, 0, K_SEM_MAX_LIMIT);

	k_thread_create(&drv_data->thread, drv_data->thread_stack,
			CONFIG_EKT2101_THREAD_STACK_SIZE,
			ekt2101_thread, drv_data,
			NULL, NULL, K_PRIO_COOP(CONFIG_EKT2101_THREAD_PRIORITY),
			0, K_NO_WAIT);
#elif defined(CONFIG_EKT2101_TRIGGER_GLOBAL_THREAD)
	drv_data->work.handler = ekt2101_work_cb;
#endif

	return 0;
}
