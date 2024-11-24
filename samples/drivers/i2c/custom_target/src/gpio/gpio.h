/*
 * Copyright (c) 2024 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPIO_H
#define GPIO_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#define digital_read(input) gpio_pin_get_dt(input)
#define digital_write(output, val) gpio_pin_set_dt(output, val)

/* Device Tree interface for Relay.  */
extern struct gpio_dt_spec leds[CONFIG_SWITCH];

/* GPIO Direction Control  */
int8_t pin_mode(struct gpio_dt_spec *user_gpio, uint32_t dir);

#endif
