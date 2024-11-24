/*
 * Copyright (c) 2024 Open Pixel Systems
 * Copyright (c) 2024 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gpio.h"
#include "i2c_slave.h"

LOG_MODULE_REGISTER(i2c_log, LOG_LEVEL_DBG);

static const struct device *bus = DEVICE_DT_GET(DT_ALIAS(i2c));
static uint8_t last_byte;

/*
 * @brief Callback which is called when a write request is received from the master.
 * @param config Pointer to the target configuration.
 */
int sample_target_write_requested_cb(struct i2c_target_config *config)
{
	LOG_DBG("sample target write requested");
	return 0;
}

/*
 * @brief Callback which is called when a write is received from the master.
 * @param config Pointer to the target configuration.
 * @param val The byte received from the master.
 */
int sample_target_write_received_cb(struct i2c_target_config *config, uint8_t val)
{
	LOG_DBG("sample target write received: val = 0x%02x, last_byte = 0x%02x", val, last_byte);
	
	if(last_byte == CONFIG_I2C_SWITCH_ADDR && val == 1){
		digital_write(&leds[0], 1);
		LOG_DBG("digital_write: HIGH");
	}
	else if(last_byte == CONFIG_I2C_SWITCH_ADDR && val == 0){
		digital_write(&leds[0], 0);
		LOG_DBG("digital_write: LOW");
	}
		
	last_byte = val;
	return 0;
}

/*
 * @brief Callback which is called when a read request is received from the master.
 * @param config Pointer to the target configuration.
 * @param val Pointer to the byte to be sent to the master.
 */
int sample_target_read_requested_cb(struct i2c_target_config *config, uint8_t *val)
{
	LOG_DBG("sample target read request: 0x%02x", *val);
	*val = 0x42;
	return 0;
}

/*
 * @brief Callback which is called when a read is processed from the master.
 * @param config Pointer to the target configuration.
 * @param val Pointer to the next byte to be sent to the master.
 */
int sample_target_read_processed_cb(struct i2c_target_config *config, uint8_t *val)
{
	LOG_DBG("sample target read processed: 0x%02x", *val);
	*val = 0x43;
	return 0;
}

/*
 * @brief Callback which is called when the master sends a stop condition.
 * @param config Pointer to the target configuration.
 */
int sample_target_stop_cb(struct i2c_target_config *config)
{
	LOG_DBG("sample target stop callback");
	return 0;
}

static struct i2c_target_callbacks sample_target_callbacks = {
	.write_requested = sample_target_write_requested_cb,
	.write_received = sample_target_write_received_cb,
	.read_requested = sample_target_read_requested_cb,
	.read_processed = sample_target_read_processed_cb,
	.stop = sample_target_stop_cb,
};

int8_t init_i2c_slave(void)
{
	struct i2c_target_config target_cfg = {
		.address = CONFIG_I2C_SLAVE_ADDR,
		.callbacks = &sample_target_callbacks,
	};

	LOG_DBG("i2c custom target sample");

	if (i2c_target_register(bus, &target_cfg) < 0) {
		LOG_ERR("Failed to register target");
		return -1;
	}

	while (1)
		k_usleep(1);

	return 0;
}