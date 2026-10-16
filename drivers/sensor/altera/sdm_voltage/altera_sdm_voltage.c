/*
 * SPDX-FileCopyrightText: Copyright (c) 2026, Altera Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Voltage sensors of the Altera SoC FPGA Secure Device Manager (SDM), read
 * with the SDM mailbox GET_VOLTAGE command.
 */

#define DT_DRV_COMPAT altr_socfpga_sdm_voltage

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <altera_sdm.h>

LOG_MODULE_REGISTER(altera_sdm_voltage, CONFIG_SENSOR_LOG_LEVEL);

#define SDM_CMD_GET_VOLTAGE 0x18U

/* The voltage is an unsigned fixed point number with 16 fractional bits */
#define SDM_VOLTAGE_FRAC_BITS 16U

#define SDM_MAX_CHANNEL 15U

struct sdm_voltage_config {
	const struct device *sdm;
	uint32_t channel;
};

struct sdm_voltage_data {
	uint32_t raw;
};

static int sdm_voltage_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct sdm_voltage_config *config = dev->config;
	struct sdm_voltage_data *data = dev->data;
	uint32_t mask = BIT(config->channel);
	uint32_t resp;
	size_t resp_len = 1U;
	int ret;

	if ((chan != SENSOR_CHAN_ALL) && (chan != SENSOR_CHAN_VOLTAGE)) {
		return -ENOTSUP;
	}

	ret = altera_sdm_mailbox_send(config->sdm, SDM_CMD_GET_VOLTAGE, &mask, 1U, &resp,
				      &resp_len);
	if (ret != 0) {
		return ret;
	}

	if (resp_len != 1U) {
		LOG_ERR("Unexpected response length %zu", resp_len);
		return -EIO;
	}

	data->raw = resp;

	return 0;
}

static int sdm_voltage_channel_get(const struct device *dev, enum sensor_channel chan,
				   struct sensor_value *val)
{
	struct sdm_voltage_data *data = dev->data;
	uint32_t frac = data->raw & BIT_MASK(SDM_VOLTAGE_FRAC_BITS);

	if (chan != SENSOR_CHAN_VOLTAGE) {
		return -ENOTSUP;
	}

	val->val1 = (int32_t)(data->raw >> SDM_VOLTAGE_FRAC_BITS);
	val->val2 = (int32_t)(((uint64_t)frac * 1000000U) >> SDM_VOLTAGE_FRAC_BITS);

	return 0;
}

static DEVICE_API(sensor, sdm_voltage_api) = {
	.sample_fetch = sdm_voltage_sample_fetch,
	.channel_get = sdm_voltage_channel_get,
};

static int sdm_voltage_init(const struct device *dev)
{
	const struct sdm_voltage_config *config = dev->config;

	if (!device_is_ready(config->sdm)) {
		LOG_ERR_DEVICE_NOT_READY(config->sdm);
		return -ENODEV;
	}

	return 0;
}

#define SDM_VOLTAGE_DEFINE(inst)                                                                   \
	BUILD_ASSERT(DT_INST_PROP(inst, channel) <= SDM_MAX_CHANNEL,                               \
		     "SDM voltage channel out of range");                                          \
                                                                                                   \
	static const struct sdm_voltage_config sdm_voltage_config_##inst = {                       \
		.sdm = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                        \
		.channel = DT_INST_PROP(inst, channel),                                            \
	};                                                                                         \
                                                                                                   \
	static struct sdm_voltage_data sdm_voltage_data_##inst;                                    \
                                                                                                   \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, sdm_voltage_init, NULL, &sdm_voltage_data_##inst,       \
				     &sdm_voltage_config_##inst, POST_KERNEL,                      \
				     CONFIG_SENSOR_INIT_PRIORITY, &sdm_voltage_api);

DT_INST_FOREACH_STATUS_OKAY(SDM_VOLTAGE_DEFINE)
