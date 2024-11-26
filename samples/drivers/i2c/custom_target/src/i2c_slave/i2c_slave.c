/*
 * Copyright (c) 2024 Open Pixel Systems
 * Copyright (c) 2024 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "adc.h"
#include "gpio.h"
#include "i2c_slave.h"

LOG_MODULE_REGISTER(i2c_log, LOG_LEVEL_DBG);

#define ADC_HIGH_BYTE(x) ((x >> 8) & 0x00FF)
#define ADC_LOW_BYTE(x) (x  & 0x00FF)

static const struct device *bus = DEVICE_DT_GET(DT_ALIAS(i2c));
static uint8_t last_byte;
static bool select_adc_ch0;
static bool select_adc_ch1;
static uint16_t adc_ch0;
static uint16_t adc_ch1;

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

	/* ADC request handler */
	if(last_byte < CONFIG_SWITCH_ADDR && val == 0){
		select_adc_ch0 = 1;
		LOG_DBG("ADC CH0 Selected");
	}
	else if(last_byte < CONFIG_SWITCH_ADDR && val == 1){
		select_adc_ch1 = 1;
		LOG_DBG("ADC CH1 Selected");
	}
	
	/* Switch request handler */
	if(last_byte == CONFIG_SWITCH_ADDR && val == 0){
		digital_write(&leds[0], 0);
		LOG_DBG("digital_write: LOW");
	}
	else if(last_byte == CONFIG_SWITCH_ADDR && val == 1){
		digital_write(&leds[0], 1);
		LOG_DBG("digital_write: HIGH");
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

	if(select_adc_ch0 == 1) {
		*val = ADC_LOW_BYTE(adc_ch0);
		LOG_DBG("ADC CH: %d, LOW_BYTE: 0x%02x", 0,  *val);
	}
	else if(select_adc_ch1 == 1) {
		*val = ADC_LOW_BYTE(adc_ch1);
		LOG_DBG("ADC CH: %d, LOW_BYTE: 0x%02x", 1,  *val);
	}

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

	if(select_adc_ch0 == 1) {
		*val = ADC_HIGH_BYTE(adc_ch0);
		select_adc_ch0 = 0;
		LOG_DBG("ADC CH: %d, HIGH_BYTE: 0x%02x", 0,  *val);
	}
	else if(select_adc_ch1 == 1) {
		*val = ADC_HIGH_BYTE(adc_ch1);
		select_adc_ch1 = 0;
		LOG_DBG("ADC CH: %d, HIGH_BYTE: 0x%02x", 1,  *val);
	}

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
	LOG_DBG("i2c custom target sample");
	
	struct i2c_target_config target_cfg = {
		.address = CONFIG_I2C_SLAVE_ADDR,
		.callbacks = &sample_target_callbacks,
	};

	if (i2c_target_register(bus, &target_cfg) < 0) {
		LOG_ERR("Failed to register target");
		return -1;
	}

	while (1) {

		if(select_adc_ch0 == 1) {
			adc_ch0 = read_adc(0);
			LOG_DBG("ADC CH: %d, value: %d", 0,  adc_ch0);
		}
		else if(select_adc_ch1 == 1) {
			adc_ch1 = read_adc(1);
			LOG_DBG("ADC CH: %d, value: %d", 0,  adc_ch0);
		}
		k_usleep(1);
	}
	return 0;
}