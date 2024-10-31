/*
 * Copyright (c) 2024 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SENSOR_EKT2101_
#define __SENSOR_EKT2101_

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/types.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define EKT2101_PACKET_SIZE 0x04
#define EKT2101_RESERVE_BIT 0x01
#define EKT2101_PID_3		0x53
#define EKT2101_PID_4		0x54

#define EKT2101_READ_BUTTON_STATUS		0x01
#define EKT2101_DIRECT_KEY_IO_SETTING	0x02
#define EKT2101_OPERATION_SETTING		0x03
#define EKT2101_TP_SENSIVITY_SETTING	0x04
#define EKT2101_PWR_SAVING_SETTING		0x05
#define EKT2101_TP_CALIBRATION_SETTING	0x06

struct ekt2101_reg_data {
    uint8_t pid     : 8;
    uint8_t reg     : 4;
    uint32_t data   : 18;
    uint8_t reserve : 2;
} __attribute__((packed));

struct ekt2101_data {
	struct ekt2101_reg_data hw_data;

	struct gpio_callback gpio_cb;
	sensor_trigger_handler_t th_handler;
	const struct sensor_trigger *th_trigger;

	const struct device *dev;

#if defined(CONFIG_EKT2101_TRIGGER_OWN_THREAD)
	K_KERNEL_STACK_MEMBER(thread_stack, CONFIG_EKT2101_THREAD_STACK_SIZE);
	struct k_sem gpio_sem;
	struct k_thread thread;
#elif defined(CONFIG_EKT2101_TRIGGER_GLOBAL_THREAD)
		struct k_work work;
#endif
};

struct ekt2101_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec interrupt;
};

int ekt2101_trigger_set(const struct device *dev,
			const struct sensor_trigger *trig,
			sensor_trigger_handler_t handler);

int ekt2101_init_interrupt(const struct device *dev);

#endif /* _SENSOR_EKT2101_ */
