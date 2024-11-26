/*
 * Copyright (c) 2024 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "adc/adc.h"
#include "gpio/gpio.h"
#include "i2c_slave/i2c_slave.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main_app, LOG_LEVEL_DBG);

int main(void)
{
	if (init_adc()){
		LOG_ERR("Failed to initialize ADC");
		return -1;
	}

	if (pin_mode(&leds[0], GPIO_OUTPUT)){
		LOG_ERR("Failed to setup pin mode");
		return -1;
	}

	if (init_i2c_slave()){
		LOG_ERR("Failed to initialize i2c_slave");
		return -1;
	}

	return 0;
}
