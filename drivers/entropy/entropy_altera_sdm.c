/*
 * SPDX-FileCopyrightText: Copyright (c) 2026, Altera Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Random number generator of the Altera SoC FPGA Secure Device Manager
 * (SDM), read with the SDM mailbox FCS RANDOM_GEN command.
 */

#define DT_DRV_COMPAT altr_socfpga_sdm_rng

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <altera_sdm.h>

LOG_MODULE_REGISTER(entropy_altera_sdm, CONFIG_ENTROPY_LOG_LEVEL);

BUILD_ASSERT(CONFIG_ENTROPY_ALTERA_SDM_INIT_PRIORITY > CONFIG_ALTERA_SDM_INIT_PRIORITY,
	     "SDM RNG must be initialized after the SDM");

#define SDM_CMD_FCS_RANDOM_GEN 0x80U

/* Each RANDOM_GEN command returns 32 bytes */
#define SDM_RNG_WORDS 8U

struct sdm_rng_config {
	const struct device *sdm;
};

static int sdm_rng_get_entropy(const struct device *dev, uint8_t *buffer, uint16_t length)
{
	const struct sdm_rng_config *config = dev->config;
	uint32_t resp[SDM_RNG_WORDS];
	size_t resp_len;
	size_t chunk;
	int ret = 0;

	while (length > 0U) {
		resp_len = ARRAY_SIZE(resp);

		ret = altera_sdm_mailbox_send(config->sdm, SDM_CMD_FCS_RANDOM_GEN, NULL, 0U, resp,
					      &resp_len);
		if (ret != 0) {
			break;
		}

		if (resp_len != SDM_RNG_WORDS) {
			LOG_ERR("Unexpected response length %zu", resp_len);
			ret = -EIO;
			break;
		}

		chunk = MIN((size_t)length, sizeof(resp));
		memcpy(buffer, resp, chunk);
		buffer += chunk;
		length -= chunk;
	}

	memset(resp, 0, sizeof(resp));

	return ret;
}

static DEVICE_API(entropy, sdm_rng_api) = {
	.get_entropy = sdm_rng_get_entropy,
};

static int sdm_rng_init(const struct device *dev)
{
	const struct sdm_rng_config *config = dev->config;

	if (!device_is_ready(config->sdm)) {
		LOG_ERR_DEVICE_NOT_READY(config->sdm);
		return -ENODEV;
	}

	return 0;
}

#define SDM_RNG_DEFINE(inst)                                                                       \
	static const struct sdm_rng_config sdm_rng_config_##inst = {                               \
		.sdm = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                        \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, sdm_rng_init, NULL, NULL, &sdm_rng_config_##inst, POST_KERNEL, \
			      CONFIG_ENTROPY_ALTERA_SDM_INIT_PRIORITY, &sdm_rng_api);

DT_INST_FOREACH_STATUS_OKAY(SDM_RNG_DEFINE)
