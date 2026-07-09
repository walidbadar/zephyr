/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_USB_SDHCI_H
#define ZEPHYR_DRIVERS_USB_SDHCI_H

#include <stdint.h>
#include <zephyr/device.h>
#include <sdhci_hw.h>

/* Host quirks per driver instance */
struct sdhci_host_quirks {
        /* Full override, not pre/post -- Cadence PHY may need to own clock+DLL together */
        int (*set_clock)(const struct device *dev);
        /* PHY/DLL delay-line programming, separate from clock and tuning */
        int (*set_delay)(const struct device *dev);
        int (*config_dll)(const struct device *dev);
};

/* Driver configuration per instance */
struct sdhci_config {
#if DT_ANY_INST_ON_BUS_STATUS_OKAY(pcie)
	struct pcie_dev *pcie;
#else
	DEVICE_MMIO_ROM;
#endif
	/* Host specific quirks */
	const struct sdhci_host_quirks *const quirks;
	void *quirk_data;
	const void *quirk_config;
	
	uint32_t max_bus_freq;
	uint32_t min_bus_freq;
	uint32_t power_delay_ms;
	uint8_t hs200_mode: 1;
	uint8_t hs400_mode: 1;
	uint8_t dw_4bit: 1;
	uint8_t dw_8bit: 1;

	void (*config_func)(const struct device *dev);
};

#define SDHCI_HOST_QUIRK_CONFIG(dev) \
	(((const struct sdhci_config *)dev->config)->quirk_config)

#define SDHCI_HOST_QUIRK_DATA(dev) \
	(((const struct sdhci_config *)dev->config)->quirk_data)

#if DT_HAS_COMPAT_STATUS_OKAY(cdns_sdhc)
#include "sdhci_cadence.h"
#endif

#define SDHCI_VENDOR_QUIRK_GET(n)                                                                  \
	COND_CODE_1(DT_NODE_VENDOR_HAS_IDX(DT_DRV_INST(n), 1),			\
			(&sdhci_host_quirks_##n),				\
			(NULL))

#define SDHCI_HOST_QUIRK_FUNC_DEFINE(fname)                                                        \
	static inline int sdhci_quirk_##fname(const struct device *const dev)                      \
	{                                                                                          \
		const struct sdhci_config *const config = dev->config;                             \
		const struct sdhci_host_quirks *const quirks = config->quirks;                     \
                                                                                                   \
		if (quirks != NULL && quirks->fname != NULL) {                                     \
			return config->quirks->fname(dev);                                         \
		}                                                                                  \
		return 0;                                                                          \
	}

SDHCI_HOST_QUIRK_FUNC_DEFINE(set_clock)
SDHCI_HOST_QUIRK_FUNC_DEFINE(set_delay)
SDHCI_HOST_QUIRK_FUNC_DEFINE(config_dll)

#endif /* ZEPHYR_DRIVERS_USB_SDHCI_H */
