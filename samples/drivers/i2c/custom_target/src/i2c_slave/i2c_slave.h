/*
 * Copyright (c) 2024 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef I2C_SLAVE_H
#define I2C_SLAVE_H

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

/* Initilaize i2c slave  */
int8_t init_i2c_slave(void);

#endif
