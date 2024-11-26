/*
 * Copyright (c) 2020 Libre Solar Technologies GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "adc.h"

LOG_MODULE_REGISTER(adc_log, LOG_LEVEL_DBG);

int8_t init_adc(void)
{
	int8_t ret=0;
	
	/* Configure channels individually prior to sampling. */
	for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++) {
		if (!adc_is_ready_dt(&adc_channels[i])) {
			LOG_ERR("ADC controller device %s not ready", adc_channels[i].dev->name);
			return -1;
		}

		ret = adc_channel_setup_dt(&adc_channels[i]);
		if (ret < 0) {
			LOG_ERR("Could not setup channel #%d (%d)", i, ret);
			return ret;
		}
	}

	return 0;
}

int read_adc(uint8_t ch)
{
	int ret=0;
	uint16_t buf;

	const struct adc_sequence_options options = {
		.interval_us = 1,
	};

	struct adc_sequence sequence = {
		.buffer = &buf,
		/* buffer size in bytes, not number of samples */
		.buffer_size = sizeof(buf),
		.options = &options,
	};

	ret = adc_sequence_init_dt(&adc_channels[ch], &sequence);
	if (ret < 0) {
		LOG_ERR("Could not init adc sequence (%d)", ret);
		return 0;
	}

	ret = adc_read_dt(&adc_channels[ch], &sequence);
	if (ret < 0) {
		LOG_ERR("Could not read (%d)", ret);
		return 0;
	}
	
	return (int)buf;
}