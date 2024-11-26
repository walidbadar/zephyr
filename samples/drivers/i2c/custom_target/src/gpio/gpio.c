/*
 * Copyright (c) 2024 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gpio.h"

LOG_MODULE_REGISTER(gpio_log, LOG_LEVEL_DBG);

/* Device Tree interface for swicth.  */
struct gpio_dt_spec leds[CONFIG_SWITCH] = {
    GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0}),
};

int8_t pin_mode(struct gpio_dt_spec *user_gpio, uint32_t dir) {
    
    int8_t ret=0;

    if (!gpio_is_ready_dt(user_gpio)) {
        LOG_ERR("Error: GPIO device %s is not ready", user_gpio->port->name);
        return -1;
    }

    ret = gpio_pin_configure_dt(user_gpio, dir);
    if (ret != 0) {
        LOG_ERR("Error %d: failed to configure %s pin %d", ret, user_gpio->port->name, user_gpio->pin);
        return ret;
    }

    return ret;
}