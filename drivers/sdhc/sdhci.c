/*
 * Copyright (c) 2023 Intel Corporation
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NOTE ON THIS REWRITE
 * --------------------
 * The register-layout side of this driver has been ported from the old
 * MMIO-mapped `struct sdhci_reg` (one field per register) to the flat
 * byte-offset macros in "sdhci.h". The new header follows the real SDHCI
 * specification more closely, which merges several of the old driver's
 * separate registers into single 32-bit words:
 *
 *   - Command (bits 31:16) and Transfer Mode (bits 15:0) -> SDHCI_XFER_MODE
 *   - Block Size (bits 11:0) and Block Count (bits 31:16) -> SDHCI_BLOCK_SIZE
 *   - Clock Control, Timeout Control and Software Reset  -> SDHCI_CLOCK_CTRL
 *   - Normal + Error Interrupt Status/Enable/Signal-Enable share one
 *     32-bit register each instead of two 16-bit halves
 *   - Capabilities is now two 32-bit registers (SDHCI_CAPS1/SDHCI_CAPS2)
 *     instead of one 64-bit register
 *   - Response 0..3 are four 32-bit registers instead of eight 16-bit ones
 *   - Power control / bus-voltage-select now live inside SDHCI_HOST_CTRL
 *   - Bus power is only reachable through SDHCI_AUTO_CMD_HOST_CTRL2 for
 *     1.8V signaling and tuning bits
 *
 * All non-register-layout constants (ADMA descriptor bit layout, clock
 * frequency targets, timeout values, UHS mode select values, etc.) are
 * still pulled in from "intel_sdhci_host.h" since sdhci.h only defines the
 * SDHCI register map, not driver-private constants.
 */

#define DT_DRV_COMPAT zephyr_sdhci

#include "sdhci.h"

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/sdhc.h>
#include <zephyr/sd/sd_spec.h>
#include <zephyr/sys/sys_io.h>

#if DT_ANY_INST_ON_BUS_STATUS_OKAY(pcie)
BUILD_ASSERT(IS_ENABLED(CONFIG_PCIE), "DT need CONFIG_PCIE");
#include <zephyr/drivers/pcie/pcie.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sdhci_hc, CONFIG_SDHC_LOG_LEVEL);

#ifdef CONFIG_SDHCI_HOST_ADMA_DESC_SIZE
#define ADMA_DESC_SIZE CONFIG_SDHCI_HOST_ADMA_DESC_SIZE
#else
#define ADMA_DESC_SIZE 0
#endif

#define SDHCI_HOST_MAX_TIMEOUT 0xe
#define SDHCI_HOST_MSEC_DELAY  1000

struct sdhci_data {
	DEVICE_MMIO_RAM;
	uint32_t rca;
	struct sdhc_io host_io;
	struct k_sem lock;
	struct k_event irq_event;
	uint64_t desc_table[ADMA_DESC_SIZE];
	struct sdhc_host_props props;
	bool card_present;
};

/* Register access helpers over the flat sdhci.h byte offsets */
#define SDHCI_REG_READ(dev, offset)         sys_read32(DEVICE_MMIO_GET(dev) + (offset))
#define SDHCI_REG_WRITE(dev, offset, value) sys_write32((value), DEVICE_MMIO_GET(dev) + (offset))

#define SDHCI_SET_BITS(dev, offset, value)	sys_set_bits(DEVICE_MMIO_GET(dev) + (offset), (value))
#define SDHCI_CLEAR_BITS(dev, offset, value)	sys_clear_bits(DEVICE_MMIO_GET(dev) + (offset), (value))

static inline void sdhci_reg_update(const struct device *dev, uint32_t offset, uint32_t mask,
				    uint32_t pos, uint32_t val)
{
	uint32_t reg = SDHCI_REG_READ(dev, offset);

	reg &= ~mask;
	reg |= (val << pos) & mask;
	SDHCI_REG_WRITE(dev, offset, reg);
}

static void enable_interrupts(const struct device *dev)
{
	SDHCI_REG_WRITE(dev, SDHCI_INT_ENABLE,
			SDHCI_NORMAL_INTERRUPT_MASK | SDHCI_ERROR_INTERRUPT_MASK);
	SDHCI_REG_WRITE(dev, SDHCI_INT_SIGNAL_ENABLE,
			SDHCI_NORMAL_INTERRUPT_MASK | SDHCI_ERROR_INTERRUPT_MASK);
	sdhci_reg_update(dev, SDHCI_CLOCK_CTRL, SDHCI_CLOCK_CTRL_DTCV_MASK,
			 SDHCI_CLOCK_CTRL_DTCV_POS, SDHCI_HOST_MAX_TIMEOUT);
}

static void disable_interrupts(const struct device *dev)
{
	/* Keep status-enable bits set so the status register still updates */
	SDHCI_REG_WRITE(dev, SDHCI_INT_ENABLE,
			SDHCI_NORMAL_INTERRUPT_MASK | SDHCI_ERROR_INTERRUPT_MASK);

	/* Disable only interrupt signal (IRQ) generation */
	SDHCI_REG_WRITE(dev, SDHCI_INT_SIGNAL_ENABLE, 0);

	sdhci_reg_update(dev, SDHCI_CLOCK_CTRL, SDHCI_CLOCK_CTRL_DTCV_MASK,
			 SDHCI_CLOCK_CTRL_DTCV_POS, SDHCI_HOST_MAX_TIMEOUT);
}

static void clear_interrupts(const struct device *dev)
{
	SDHCI_REG_WRITE(dev, SDHCI_INT_STATUS,
			SDHCI_NORMAL_INTERRUPT_MASK | SDHCI_ERROR_INTERRUPT_MASK);
}

static int sdhci_set_voltage(const struct device *dev, enum sd_voltage signal_voltage)
{
	uint32_t caps1 = SDHCI_REG_READ(dev, SDHCI_CAPS1);
	uint32_t host_ctrl = SDHCI_REG_READ(dev, SDHCI_HOST_CTRL);
	bool power_state = (host_ctrl & SDHCI_HOST_CTRL_BP) ? true : false;
	int ret = 0;

	if (power_state) {
		/* Turn OFF Bus Power before config clock */
		SDHCI_CLEAR_BITS(dev, SDHCI_HOST_CTRL, SDHCI_HOST_CTRL_BP);
	}

	switch (signal_voltage) {
	case SD_VOL_3_3_V:
		if (caps1 & SDHCI_CAPS1_VS33) {
			SDHCI_CLEAR_BITS(dev, SDHCI_AUTO_CMD_HOST_CTRL2, SDHCI_HOST_CTRL2_V18SE);

			/* 3.3v voltage select */
			sdhci_reg_update(dev, SDHCI_HOST_CTRL, SDHCI_HOST_CTRL_BVS_MASK,
					 SDHCI_HOST_CTRL_BVS_POS, SDHCI_HOST_VOL_3_3_V_SELECT);
			LOG_DBG("3.3V Selected for MMC Card");
		} else {
			LOG_ERR("3.3V not supported by MMC Host");
			ret = -ENOTSUP;
		}
		break;

	case SD_VOL_3_0_V:
		if (caps1 & SDHCI_CAPS1_VS30) {
			SDHCI_CLEAR_BITS(dev, SDHCI_AUTO_CMD_HOST_CTRL2, SDHCI_HOST_CTRL2_V18SE);

			/* 3.0v voltage select */
			sdhci_reg_update(dev, SDHCI_HOST_CTRL, SDHCI_HOST_CTRL_BVS_MASK,
					 SDHCI_HOST_CTRL_BVS_POS, SDHCI_HOST_VOL_3_0_V_SELECT);
			LOG_DBG("3.0V Selected for MMC Card");
		} else {
			LOG_ERR("3.0V not supported by MMC Host");
			ret = -ENOTSUP;
		}
		break;

	case SD_VOL_1_8_V:
		if (caps1 & SDHCI_CAPS1_VS18) {
			SDHCI_SET_BITS(dev, SDHCI_AUTO_CMD_HOST_CTRL2, SDHCI_HOST_CTRL2_V18SE);

			/* 1.8v voltage select */
			sdhci_reg_update(dev, SDHCI_HOST_CTRL, SDHCI_HOST_CTRL_BVS_MASK,
					 SDHCI_HOST_CTRL_BVS_POS, SDHCI_HOST_VOL_1_8_V_SELECT);
			LOG_DBG("1.8V Selected for MMC Card");
		} else {
			LOG_ERR("1.8V not supported by MMC Host");
			ret = -ENOTSUP;
		}
		break;

	default:
		ret = -EINVAL;
	}

	if (power_state) {
		/* Turn ON Bus Power */
		SDHCI_SET_BITS(dev, SDHCI_HOST_CTRL, SDHCI_HOST_CTRL_BP);
	}

	return ret;
}

static void sdhci_set_power(const struct device *dev, enum sdhc_power state)
{
	if (state == SDHC_POWER_ON) {
		/* Turn ON Bus Power */
		SDHCI_SET_BITS(dev, SDHCI_HOST_CTRL, SDHCI_HOST_CTRL_BP);
	} else {
		/* Turn OFF Bus Power */
		SDHCI_CLEAR_BITS(dev, SDHCI_HOST_CTRL, SDHCI_HOST_CTRL_BP);
	}

	k_msleep(10u);
}

static int sdhci_clock_ctrl(const struct device *dev, bool enable)
{
	uint32_t pstate = SDHCI_REG_READ(dev, SDHCI_PRESENT_STATE);
	uint32_t clk;

	if (pstate & SDHCI_PRESENT_STATE_CICMD) {
		LOG_ERR("present_state:%x", pstate);
		return false;
	}
	if (pstate & SDHCI_PRESENT_STATE_CIDAT) {
		LOG_ERR("present_state:%x", pstate);
		return false;
	}

	if (enable) {
		SDHCI_SET_BITS(dev, SDHCI_CLOCK_CTRL, SDHCI_CLOCK_CTRL_ICE);
		while ((SDHCI_REG_READ(dev, SDHCI_CLOCK_CTRL) & SDHCI_CLOCK_CTRL_ICS) == 0) {
			;
		}

		SDHCI_SET_BITS(dev, SDHCI_CLOCK_CTRL, SDHCI_CLOCK_CTRL_SDCE);
		while ((SDHCI_REG_READ(dev, SDHCI_CLOCK_CTRL) & SDHCI_CLOCK_CTRL_SDCE) == 0) {
			;
		}
	} else {
		SDHCI_CLEAR_BITS(dev, SDHCI_CLOCK_CTRL, SDHCI_CLOCK_CTRL_ICE | SDHCI_CLOCK_CTRL_SDCE);
		while ((SDHCI_REG_READ(dev, SDHCI_CLOCK_CTRL) & SDHCI_CLOCK_CTRL_SDCE) != 0) {
			;
		}
	}

	return 0;
}

static int sdhci_set_clock(const struct device *dev, enum sdhc_clock_speed speed)
{
	uint32_t caps1;
	uint8_t base_freq;
	uint32_t clock_divider;
	float freq;
	int ret;

	switch (speed) {
	case SDMMC_CLOCK_400KHZ:
		freq = SDHCI_HOST_CLK_FREQ_400K;
		break;

	case SD_CLOCK_25MHZ:
	case MMC_CLOCK_26MHZ:
		freq = SDHCI_HOST_CLK_FREQ_25M;
		break;

	case SD_CLOCK_50MHZ:
	case MMC_CLOCK_52MHZ:
		freq = SDHCI_HOST_CLK_FREQ_50M;
		break;

	case SD_CLOCK_100MHZ:
		freq = SDHCI_HOST_CLK_FREQ_100M;
		break;

	case MMC_CLOCK_HS200:
		freq = SDHCI_HOST_CLK_FREQ_200M;
		break;

	case SD_CLOCK_208MHZ:
	default:
		return false;
	}

	ret = sdhci_clock_ctrl(dev, false);
	if (ret < 0) {
		return ret;
	}

	caps1 = SDHCI_REG_READ(dev, SDHCI_CAPS1);
	base_freq = (caps1 & SDHCI_CAPS1_BCSDCLK_MASK) >> SDHCI_CAPS1_BCSDCLK_POS;
	clock_divider = (int)(base_freq / (freq * 2));

	LOG_DBG("Clock divider for MMC Clk: %d Hz is %d", speed, clock_divider);

	sdhci_reg_update(dev, SDHCI_CLOCK_CTRL, SDHCI_CLOCK_CTRL_SDCFSL_MASK,
			 SDHCI_CLOCK_CTRL_SDCFSL_POS, clock_divider);
	sdhci_reg_update(dev, SDHCI_CLOCK_CTRL, SDHCI_CLOCK_CTRL_SDCFSH_MASK,
			 SDHCI_CLOCK_CTRL_SDCFSH_POS, clock_divider >> 8);

	return sdhci_clock_ctrl(dev, true);
}

static int sdhci_set_timing(const struct device *dev, enum sdhc_timing_mode timing)
{
	int ret = 0;
	uint8_t mode;

	LOG_DBG("UHS Mode: %d", timing);

	switch (timing) {
	case SDHC_TIMING_LEGACY:
	case SDHC_TIMING_HS:
	case SDHC_TIMING_SDR12:
		mode = SDHCI_HOST_UHSMODE_SDR12;
		break;

	case SDHC_TIMING_SDR25:
		mode = SDHCI_HOST_UHSMODE_SDR25;
		break;

	case SDHC_TIMING_SDR50:
		mode = SDHCI_HOST_UHSMODE_SDR50;
		break;

	case SDHC_TIMING_SDR104:
		mode = SDHCI_HOST_UHSMODE_SDR104;
		break;

	case SDHC_TIMING_DDR50:
	case SDHC_TIMING_DDR52:
		mode = SDHCI_HOST_UHSMODE_DDR50;
		break;

	case SDHC_TIMING_HS400:
	case SDHC_TIMING_HS200:
		mode = SDHCI_HOST_UHSMODE_HS400;
		break;

	default:
		ret = -ENOTSUP;
	}

	if (!ret) {
		ret = sdhci_clock_ctrl(dev, false);
		if (ret < 0) {
			LOG_ERR("Disable clk failed");
			return ret;
		}

		SDHCI_REG_WRITE(dev, SDHCI_AUTO_CMD_HOST_CTRL2,
				SDHCI_REG_READ(dev, SDHCI_AUTO_CMD_HOST_CTRL2) |
					SDHCI_HOST_CTRL2_V18SE);
		sdhci_reg_update(dev, SDHCI_AUTO_CMD_HOST_CTRL2, SDHCI_HOST_CTRL2_UMS_MASK,
				 SDHCI_HOST_CTRL2_UMS_POS, mode);

		ret = sdhci_clock_ctrl(dev, true);
		if (ret < 0) {
			LOG_ERR("Disable clk failed");
			return ret;
		}
	}

	return ret;
}

static int wait_for_cmd_complete(struct sdhci_data *sdhci, uint32_t time_out)
{
	int ret;
	k_timeout_t wait_time;
	uint32_t events;

	if (time_out == SDHC_TIMEOUT_FOREVER) {
		wait_time = K_FOREVER;
	} else {
		wait_time = K_MSEC(time_out);
	}

	events = k_event_wait(&sdhci->irq_event,
			      SDHCI_INT_STATUS_CC |
				      ERROR_INTERRUPT_STATUS_EVENT(SDHCI_ERROR_INTERRUPT_MASK),
			      false, wait_time);

	if (events & SDHCI_INT_STATUS_CC) {
		ret = 0;
	} else if (events & ERROR_INTERRUPT_STATUS_EVENT(SDHCI_ERROR_INTERRUPT_MASK)) {
		LOG_ERR("wait for cmd complete error: %x", events);
		ret = -EIO;
	} else {
		LOG_ERR("wait for cmd complete timeout");
		ret = -EAGAIN;
	}

	return ret;
}

static int poll_cmd_complete(const struct device *dev, uint32_t time_out)
{
	int ret = -EAGAIN;
	int32_t retry = time_out;
	uint32_t int_stat;
	uint32_t adma_err;

	while (retry > 0) {
		int_stat = SDHCI_REG_READ(dev, SDHCI_INT_STATUS);
		if (int_stat & SDHCI_INT_STATUS_CC) {
			SDHCI_REG_WRITE(dev, SDHCI_INT_STATUS, SDHCI_INT_STATUS_CC);
			ret = 0;
			break;
		}

		k_busy_wait(1000u);
		retry--;
	}

	int_stat = SDHCI_REG_READ(dev, SDHCI_INT_STATUS);
	if (int_stat & SDHCI_ERROR_INTERRUPT_MASK) {
		LOG_ERR("err_int_stat:%x", int_stat & SDHCI_ERROR_INTERRUPT_MASK);
		SDHCI_REG_WRITE(dev, SDHCI_INT_STATUS, int_stat & SDHCI_ERROR_INTERRUPT_MASK);
		ret = -EIO;
	}

	if (IS_ENABLED(CONFIG_SDHCI_HOST_ADMA)) {
		adma_err = SDHCI_REG_READ(dev, SDHCI_ADMA_ERROR);
		if (adma_err) {
			LOG_ERR("adma error: %x", adma_err);
			ret = -EIO;
		}
	}
	return ret;
}

void sdhci_host_sw_reset(const struct device *dev, enum sdhci_sw_reset reset)
{
	uint32_t bit;

	if (reset == SDHCI_HOST_SW_RESET_DATA_LINE) {
		bit = SDHCI_CLOCK_CTRL_SRDAT;
	} else if (reset == SDHCI_HOST_SW_RESET_CMD_LINE) {
		bit = SDHCI_CLOCK_CTRL_SRCMD;
	} else {
		bit = SDHCI_CLOCK_CTRL_SRFA;
	}

	SDHCI_SET_BITS(dev, SDHCI_CLOCK_CTRL, bit);

	while (SDHCI_REG_READ(dev, SDHCI_CLOCK_CTRL) & bit) {
		;
	}

	k_sleep(K_MSEC(100u));
}

static int sdhci_dma_init(const struct device *dev, struct sdhc_data *data, bool read)
{
	struct sdhci_data *sdhci = dev->data;

	if (IS_ENABLED(CONFIG_DCACHE) && !read) {
		sys_cache_data_flush_range(data->data, (data->blocks * data->block_size));
	}

	if (IS_ENABLED(CONFIG_SDHCI_HOST_ADMA)) {
		uint8_t *buff = data->data;

		/* Setup DMA transfer using ADMA2 */
		memset(sdhci->desc_table, 0, sizeof(sdhci->desc_table));

#if defined(CONFIG_SDHCI_HOST_ADMA_DESC_SIZE)
		__ASSERT_NO_MSG(data->blocks < CONFIG_SDHCI_HOST_ADMA_DESC_SIZE);
#endif
		for (int i = 0; i < data->blocks; i++) {
			sdhci->desc_table[i] = ((uint64_t)buff) << SDHCI_HOST_ADMA_BUFF_ADD_LOC;
			sdhci->desc_table[i] |= data->block_size << SDHCI_HOST_ADMA_BUFF_LEN_LOC;

			if (i == (data->blocks - 1u)) {
				sdhci->desc_table[i] |= SDHCI_HOST_ADMA_BUFF_LINK_LAST;
				sdhci->desc_table[i] |= SDHCI_HOST_ADMA_INTERRUPT_EN;
				sdhci->desc_table[i] |= SDHCI_HOST_ADMA_BUFF_LAST;
			} else {
				sdhci->desc_table[i] |= SDHCI_HOST_ADMA_BUFF_LINK_NEXT;
			}
			sdhci->desc_table[i] |= SDHCI_HOST_ADMA_BUFF_VALID;
			buff += data->block_size;
			LOG_DBG("desc_table:%llx", sdhci->desc_table[i]);
		}

		SDHCI_REG_WRITE(dev, SDHCI_ADMA_SYS_ADDR1,
				(uint32_t)((uintptr_t)sdhci->desc_table & ADDRESS_32BIT_MASK));
		SDHCI_REG_WRITE(
			dev, SDHCI_ADMA_SYS_ADDR2,
			(uint32_t)(((uintptr_t)sdhci->desc_table >> 32) & ADDRESS_32BIT_MASK));

		LOG_DBG("adma: %llx %x %p", sdhci->desc_table[0],
			SDHCI_REG_READ(dev, SDHCI_ADMA_SYS_ADDR1), sdhci->desc_table);
	} else {
		/* Setup DMA transfer using SDMA */
		SDHCI_REG_WRITE(dev, SDHCI_DMA_ADDRESS, (uint32_t)((uintptr_t)data->data));
		LOG_DBG("sdma_sysaddr: %x", SDHCI_REG_READ(dev, SDHCI_DMA_ADDRESS));
	}
	return 0;
}

static int sdhci_init_xfr(const struct device *dev, struct sdhc_data *data, bool read)
{
	struct sdhci_data *sdhci = dev->data;
	uint16_t multi_block = 0u;
	uint32_t xfer;

	if (IS_ENABLED(CONFIG_SDHCI_HOST_DMA)) {
		sdhci_dma_init(dev, data, read);
	}

	if (IS_ENABLED(CONFIG_SDHCI_HOST_ADMA)) {
		sdhci_reg_update(dev, SDHCI_HOST_CTRL, SDHCI_HOST_CTRL_DMASEL_MASK,
				 SDHCI_HOST_CTRL_DMASEL_POS, 2u);
	} else {
		sdhci_reg_update(dev, SDHCI_HOST_CTRL, SDHCI_HOST_CTRL_DMASEL_MASK,
				 SDHCI_HOST_CTRL_DMASEL_POS, 0u);
	}

	/* Block Size / SDMA buffer boundary: both live in SDHCI_BLOCK_SIZE now */
	sdhci_reg_update(dev, SDHCI_BLOCK_SIZE, SDHCI_BLOCK_SIZE_SDMABB_MASK,
			 SDHCI_BLOCK_SIZE_SDMABB_POS, SDHCI_HOST_SDMA_BOUNDARY);
	sdhci_reg_update(dev, SDHCI_BLOCK_SIZE, SDHCI_BLOCK_SIZE_TBS_MASK, SDHCI_BLOCK_SIZE_TBS_POS,
			 data->block_size);

	if (data->blocks > 1) {
		multi_block = 1u;
	}

	if (IS_ENABLED(CONFIG_SDHCI_HOST_AUTO_STOP)) {
		if (IS_ENABLED(CONFIG_SDHCI_HOST_ADMA) &&
		    sdhci->host_io.timing == SDHC_TIMING_SDR104) {
			/* Auto cmd23 only applicable for ADMA */
			sdhci_reg_update(dev, SDHCI_XFER_MODE, SDHCI_XFER_MODE_ACE_MASK,
					 SDHCI_XFER_MODE_ACE_POS, multi_block ? 2 : 0);
		} else {
			sdhci_reg_update(dev, SDHCI_XFER_MODE, SDHCI_XFER_MODE_ACE_MASK,
					 SDHCI_XFER_MODE_ACE_POS, multi_block ? 1 : 0);
		}
	} else {
		sdhci_reg_update(dev, SDHCI_XFER_MODE, SDHCI_XFER_MODE_ACE_MASK,
				 SDHCI_XFER_MODE_ACE_POS, 0);
	}

	if (!IS_ENABLED(CONFIG_SDHCI_HOST_AUTO_STOP)) {
		/* Set block count field to 0 for infinite transfer mode */
		sdhci_reg_update(dev, SDHCI_BLOCK_SIZE, SDHCI_BLOCK_SIZE_BCCT_MASK,
				 SDHCI_BLOCK_SIZE_BCCT_POS, 0);
		xfer = SDHCI_REG_READ(dev, SDHCI_XFER_MODE);
		xfer &= ~SDHCI_XFER_MODE_BCE;
		SDHCI_REG_WRITE(dev, SDHCI_XFER_MODE, xfer);
	} else {
		sdhci_reg_update(dev, SDHCI_BLOCK_SIZE, SDHCI_BLOCK_SIZE_BCCT_MASK,
				 SDHCI_BLOCK_SIZE_BCCT_POS, (uint16_t)data->blocks);
		/* Enable block count in transfer register */
		xfer = SDHCI_REG_READ(dev, SDHCI_XFER_MODE);
		if (multi_block) {
			xfer |= SDHCI_XFER_MODE_BCE;
		} else {
			xfer &= ~SDHCI_XFER_MODE_BCE;
		}
		SDHCI_REG_WRITE(dev, SDHCI_XFER_MODE, xfer);
	}

	xfer = SDHCI_REG_READ(dev, SDHCI_XFER_MODE);
	if (multi_block) {
		xfer |= SDHCI_XFER_MODE_MSBS;
	} else {
		xfer &= ~SDHCI_XFER_MODE_MSBS;
	}

	/* Set data transfer direction, Read = 1, Write = 0 */
	if (read) {
		xfer |= SDHCI_XFER_MODE_DTDS;
	} else {
		xfer &= ~SDHCI_XFER_MODE_DTDS;
	}

	if (IS_ENABLED(CONFIG_SDHCI_HOST_DMA)) {
		/* Enable DMA */
		xfer |= SDHCI_XFER_MODE_DMAE;
	} else {
		xfer &= ~SDHCI_XFER_MODE_DMAE;
	}
	SDHCI_REG_WRITE(dev, SDHCI_XFER_MODE, xfer);

	if (IS_ENABLED(CONFIG_SDHCI_HOST_BLOCK_GAP)) {
		/* Set an interrupt at the block gap */
		SDHCI_REG_WRITE(dev, SDHCI_HOST_CTRL,
				SDHCI_REG_READ(dev, SDHCI_HOST_CTRL) | SDHCI_HOST_CTRL_IBG);
	} else {
		SDHCI_REG_WRITE(dev, SDHCI_HOST_CTRL,
				SDHCI_REG_READ(dev, SDHCI_HOST_CTRL) & ~SDHCI_HOST_CTRL_IBG);
	}

	/* Set data timeout time (Data Timeout Counter Value field) */
	sdhci_reg_update(dev, SDHCI_CLOCK_CTRL, SDHCI_CLOCK_CTRL_DTCV_MASK,
			 SDHCI_CLOCK_CTRL_DTCV_POS, data->timeout_ms);

	return 0;
}

static int wait_xfr_intr_complete(const struct device *dev, uint32_t time_out)
{
	struct sdhci_data *sdhci = dev->data;
	uint32_t events;
	int ret;
	k_timeout_t wait_time;

	LOG_DBG("");

	if (time_out == SDHC_TIMEOUT_FOREVER) {
		wait_time = K_FOREVER;
	} else {
		wait_time = K_MSEC(time_out);
	}

	events = k_event_wait(&sdhci->irq_event,
			      SDHCI_INT_STATUS_TC |
				      ERROR_INTERRUPT_STATUS_EVENT(SDHCI_INT_STATUS_EADMA),
			      false, wait_time);

	if (events & SDHCI_INT_STATUS_TC) {
		ret = 0;
	} else if (events & ERROR_INTERRUPT_STATUS_EVENT(0xFFFF)) {
		LOG_ERR("wait for xfer complete error: %x", events);
		ret = -EIO;
	} else {
		LOG_ERR("wait for xfer complete timeout");
		ret = -EAGAIN;
	}

	return ret;
}

static int wait_xfr_poll_complete(const struct device *dev, uint32_t time_out)
{
	int ret = -EAGAIN;
	int32_t retry = time_out;

	LOG_DBG("");

	while (retry > 0) {
		if (SDHCI_REG_READ(dev, SDHCI_INT_STATUS) & SDHCI_INT_STATUS_TC) {
			SDHCI_REG_WRITE(dev, SDHCI_INT_STATUS, SDHCI_INT_STATUS_TC);
			ret = 0;
			break;
		}

		k_busy_wait(SDHCI_HOST_MSEC_DELAY);
		retry--;
	}

	return ret;
}

static int wait_xfr_complete(const struct device *dev, uint32_t time_out)
{
	int ret;

	if (IS_ENABLED(CONFIG_SDHCI_HOST_INTERRUPT)) {
		ret = wait_xfr_intr_complete(dev, time_out);
	} else {
		ret = wait_xfr_poll_complete(dev, time_out);
	}
	return ret;
}

static enum sdhci_response_type sdhci_decode_resp_type(enum sd_rsp_type type)
{
	enum sdhci_response_type resp_type;

	switch (type & 0xF) {
	case SD_RSP_TYPE_NONE:
		resp_type = SDHCI_RESP_NONE;
		break;
	case SD_RSP_TYPE_R1:
	case SD_RSP_TYPE_R3:
	case SD_RSP_TYPE_R4:
	case SD_RSP_TYPE_R5:
		resp_type = SDHCI_RESP_LEN_48;
		break;
	case SD_RSP_TYPE_R1b:
		resp_type = SDHCI_RESP_LEN_48B;
		break;
	case SD_RSP_TYPE_R2:
		resp_type = SDHCI_RESP_LEN_136;
		break;

	case SD_RSP_TYPE_R5b:
	case SD_RSP_TYPE_R6:
	case SD_RSP_TYPE_R7:
	default:
		resp_type = SDHCI_INVAL_HOST_RESP_LEN;
	}

	return resp_type;
}

static void update_cmd_response(const struct device *dev, struct sdhc_command *sdhc_cmd)
{
	uint32_t resp0, resp1, resp2, resp3;

	if (sdhc_cmd->response_type == SD_RSP_TYPE_NONE) {
		return;
	}

	resp0 = SDHCI_REG_READ(dev, SDHCI_RESPONSE0);

	if (sdhc_cmd->response_type == SD_RSP_TYPE_R2) {
		resp1 = SDHCI_REG_READ(dev, SDHCI_RESPONSE1);
		resp2 = SDHCI_REG_READ(dev, SDHCI_RESPONSE2);
		resp3 = SDHCI_REG_READ(dev, SDHCI_RESPONSE3);

		LOG_DBG("cmd resp: %x %x %x %x", resp0, resp1, resp2, resp3);

		sdhc_cmd->response[0u] = resp3;
		sdhc_cmd->response[1U] = resp2;
		sdhc_cmd->response[2U] = resp1;
		sdhc_cmd->response[3U] = resp0;
	} else {
		LOG_DBG("cmd resp: %x", resp0);
		sdhc_cmd->response[0u] = resp0;
	}
}

static int sdhci_host_send_cmd(const struct device *dev, const struct sdhci_cmd_config *config)
{
	struct sdhci_data *sdhci = dev->data;
	struct sdhc_command *sdhc_cmd = config->sdhc_cmd;
	enum sdhci_response_type resp_type = sdhci_decode_resp_type(sdhc_cmd->response_type);
	uint32_t pstate;
	uint32_t xfer;
	int ret;

	LOG_DBG("");

	pstate = SDHCI_REG_READ(dev, SDHCI_PRESENT_STATE);

	/* Check if CMD line is available */
	if (pstate & SDHCI_PRESENT_STATE_CICMD) {
		LOG_ERR("CMD line is not available");
		return -EBUSY;
	}

	if (config->data_present && (pstate & SDHCI_PRESENT_STATE_CIDAT)) {
		LOG_ERR("Data line is not available");
		return -EBUSY;
	}

	if (resp_type == SDHCI_INVAL_HOST_RESP_LEN) {
		LOG_ERR("Invalid eMMC resp type:%d", resp_type);
		return -EINVAL;
	}

	k_event_clear(&sdhci->irq_event, SDHCI_INT_STATUS_CC);

	SDHCI_REG_WRITE(dev, SDHCI_ARGUMENT, sdhc_cmd->arg);

	/*
	 * Command fields (bits 31:16) share the same 32-bit register as
	 * Transfer Mode (bits 15:0). Read-modify-write so the Transfer Mode
	 * bits programmed by sdhci_init_xfr() are preserved; the write to
	 * this register is what issues the command to the card.
	 */
	xfer = SDHCI_REG_READ(dev, SDHCI_XFER_MODE);
	xfer &= ~(SDHCI_XFER_MODE_CIDX_MASK | SDHCI_XFER_MODE_CT_MASK | SDHCI_XFER_MODE_DPS |
		  SDHCI_XFER_MODE_CICE | SDHCI_XFER_MODE_CRCCE | SDHCI_XFER_MODE_RTS_MASK);
	xfer |= (config->cmd_idx << SDHCI_XFER_MODE_CIDX_POS) & SDHCI_XFER_MODE_CIDX_MASK;
	xfer |= (config->cmd_type << SDHCI_XFER_MODE_CT_POS) & SDHCI_XFER_MODE_CT_MASK;
	xfer |= config->data_present ? SDHCI_XFER_MODE_DPS : 0;
	xfer |= config->idx_check_en ? SDHCI_XFER_MODE_CICE : 0;
	xfer |= config->crc_check_en ? SDHCI_XFER_MODE_CRCCE : 0;
	xfer |= (resp_type << SDHCI_XFER_MODE_RTS_POS) & SDHCI_XFER_MODE_RTS_MASK;
	SDHCI_REG_WRITE(dev, SDHCI_XFER_MODE, xfer);

	LOG_DBG("CMD/XFER REG:%x %x", xfer, SDHCI_REG_READ(dev, SDHCI_XFER_MODE));
	if (IS_ENABLED(CONFIG_SDHCI_HOST_INTERRUPT)) {
		ret = wait_for_cmd_complete(sdhci, sdhc_cmd->timeout_ms);
	} else {
		ret = poll_cmd_complete(dev, sdhc_cmd->timeout_ms);
	}
	if (ret) {
		LOG_ERR("Error on send cmd: %d, status:%d", config->cmd_idx, ret);
		return ret;
	}

	update_cmd_response(dev, sdhc_cmd);

	return 0;
}

static int sdhci_stop_transfer(const struct device *dev)
{
	struct sdhci_data *sdhci = dev->data;
	struct sdhc_command hdc_cmd = {0};
	struct sdhci_cmd_config cmd;

	hdc_cmd.arg = sdhci->rca << SDHCI_HOST_RCA_SHIFT;
	hdc_cmd.response_type = SD_RSP_TYPE_R1;
	hdc_cmd.timeout_ms = 1000;

	cmd.sdhc_cmd = &hdc_cmd;
	cmd.cmd_idx = SD_STOP_TRANSMISSION;
	cmd.cmd_type = SDHCI_CMD_NORMAL;
	cmd.data_present = false;
	cmd.idx_check_en = false;
	cmd.crc_check_en = false;

	return sdhci_host_send_cmd(dev, &cmd);
}

static int sdhci_reset(const struct device *dev)
{
	LOG_DBG("");

	if (!(SDHCI_REG_READ(dev, SDHCI_PRESENT_STATE) & SDHCI_PRESENT_STATE_CI)) {
		LOG_ERR("No SDHCI card found");
		return -ENODEV;
	}

	/* Reset device to idle state */
	sdhci_host_sw_reset(dev, SDHCI_HOST_SW_RESET_ALL);

	clear_interrupts(dev);

	if (IS_ENABLED(CONFIG_SDHCI_HOST_INTERRUPT)) {
		enable_interrupts(dev);
	} else {
		disable_interrupts(dev);
	}

	return 0;
}

static int read_data_port(const struct device *dev, struct sdhc_data *sdhc)
{
	struct sdhci_data *sdhci = dev->data;
	uint32_t block_size = sdhc->block_size;
	uint32_t i, block_cnt = sdhc->blocks;
	uint32_t *data = (uint32_t *)sdhc->data;
	k_timeout_t wait_time;

	if (sdhc->timeout_ms == SDHC_TIMEOUT_FOREVER) {
		wait_time = K_FOREVER;
	} else {
		wait_time = K_MSEC(sdhc->timeout_ms);
	}

	LOG_DBG("");

	while (block_cnt--) {
		if (IS_ENABLED(CONFIG_SDHCI_HOST_INTERRUPT)) {
			uint32_t events;

			events = k_event_wait(&sdhci->irq_event, SDHCI_INT_STATUS_BRR, false,
					      wait_time);
			k_event_clear(&sdhci->irq_event, SDHCI_INT_STATUS_BRR);
			if (!(events & SDHCI_INT_STATUS_BRR)) {
				LOG_ERR("time out on BUF_RD_READY:%d", (sdhc->blocks - block_cnt));
				return -EIO;
			}
		} else {
			while ((SDHCI_REG_READ(dev, SDHCI_PRESENT_STATE) &
				SDHCI_PRESENT_STATE_BRE) == 0) {
				;
			}
		}

		if (SDHCI_REG_READ(dev, SDHCI_PRESENT_STATE) & SDHCI_PRESENT_STATE_CIDAT) {
			for (i = block_size >> 2u; i != 0u; i--) {
				*data = SDHCI_REG_READ(dev, SDHCI_BUFFER);
				data++;
			}
		}
	}

	return wait_xfr_complete(dev, sdhc->timeout_ms);
}

static int write_data_port(const struct device *dev, struct sdhc_data *sdhc)
{
	struct sdhci_data *sdhci = dev->data;
	uint32_t block_size = sdhc->block_size;
	uint32_t i, block_cnt = sdhc->blocks;
	uint32_t *data = (uint32_t *)sdhc->data;
	k_timeout_t wait_time;

	if (sdhc->timeout_ms == SDHC_TIMEOUT_FOREVER) {
		wait_time = K_FOREVER;
	} else {
		wait_time = K_MSEC(sdhc->timeout_ms);
	}

	LOG_DBG("");

	while ((SDHCI_REG_READ(dev, SDHCI_PRESENT_STATE) & SDHCI_PRESENT_STATE_BWE) == 0) {
		;
	}

	while (1) {
		uint32_t events;

		if (IS_ENABLED(CONFIG_SDHCI_HOST_INTERRUPT)) {
			k_event_clear(&sdhci->irq_event, SDHCI_INT_STATUS_BWR);
		}

		if (SDHCI_REG_READ(dev, SDHCI_PRESENT_STATE) & SDHCI_PRESENT_STATE_CIDAT) {
			for (i = block_size >> 2u; i != 0u; i--) {
				SDHCI_REG_WRITE(dev, SDHCI_BUFFER, *data);
				data++;
			}
		}

		LOG_DBG("BUF_WR_READY");

		if (!(--block_cnt)) {
			break;
		}
		if (IS_ENABLED(CONFIG_SDHCI_HOST_INTERRUPT)) {
			events = k_event_wait(&sdhci->irq_event, SDHCI_INT_STATUS_BWR, false,
					      wait_time);
			k_event_clear(&sdhci->irq_event, SDHCI_INT_STATUS_BWR);

			if (!(events & SDHCI_INT_STATUS_BWR)) {
				LOG_ERR("time out on BUF_WR_READY");
				return -EIO;
			}
		} else {
			while ((SDHCI_REG_READ(dev, SDHCI_PRESENT_STATE) &
				SDHCI_PRESENT_STATE_BWE) == 0) {
				;
			}
		}
	}

	return wait_xfr_complete(dev, sdhc->timeout_ms);
}

static int sdhci_send_cmd_no_data(const struct device *dev, uint32_t cmd_idx,
				  struct sdhc_command *cmd)
{
	struct sdhci_cmd_config sdhci_cmd;

	sdhci_cmd.sdhc_cmd = cmd;
	sdhci_cmd.cmd_idx = cmd_idx;
	sdhci_cmd.cmd_type = SDHCI_CMD_NORMAL;
	sdhci_cmd.data_present = false;
	sdhci_cmd.idx_check_en = false;
	sdhci_cmd.crc_check_en = false;

	return sdhci_host_send_cmd(dev, &sdhci_cmd);
}

static int sdhci_send_cmd_data(const struct device *dev, uint32_t cmd_idx, struct sdhc_command *cmd,
			       struct sdhc_data *data, bool read)
{
	struct sdhci_cmd_config sdhci_cmd;
	int ret;

	sdhci_cmd.sdhc_cmd = cmd;
	sdhci_cmd.cmd_idx = cmd_idx;
	sdhci_cmd.cmd_type = SDHCI_CMD_NORMAL;
	sdhci_cmd.data_present = true;
	sdhci_cmd.idx_check_en = true;
	sdhci_cmd.crc_check_en = true;

	ret = sdhci_init_xfr(dev, data, read);
	if (ret) {
		LOG_ERR("Error on init xfr");
		return ret;
	}

	ret = sdhci_host_send_cmd(dev, &sdhci_cmd);
	if (ret) {
		return ret;
	}

	if (IS_ENABLED(CONFIG_SDHCI_HOST_DMA)) {
		ret = wait_xfr_complete(dev, data->timeout_ms);
	} else {
		if (read) {
			ret = read_data_port(dev, data);
		} else {
			ret = write_data_port(dev, data);
		}
	}

	return ret;
}

static int sdhci_xfr(const struct device *dev, struct sdhc_command *cmd, struct sdhc_data *data,
		     bool read)
{
	struct sdhci_data *sdhci = dev->data;
	int ret;
	struct sdhci_cmd_config sdhci_cmd;

	ret = sdhci_init_xfr(dev, data, read);
	if (ret) {
		LOG_ERR("error sdhci init xfr");
		return ret;
	}
	sdhci_cmd.sdhc_cmd = cmd;
	sdhci_cmd.cmd_type = SDHCI_CMD_NORMAL;
	sdhci_cmd.data_present = true;
	sdhci_cmd.idx_check_en = true;
	sdhci_cmd.crc_check_en = true;

	k_event_clear(&sdhci->irq_event, SDHCI_INT_STATUS_TC);
	k_event_clear(&sdhci->irq_event, read ? SDHCI_INT_STATUS_BRR : SDHCI_INT_STATUS_BWR);

	if (data->blocks > 1) {
		sdhci_cmd.cmd_idx = read ? SD_READ_MULTIPLE_BLOCK : SD_WRITE_MULTIPLE_BLOCK;
		ret = sdhci_host_send_cmd(dev, &sdhci_cmd);
	} else {
		sdhci_cmd.cmd_idx = read ? SD_READ_SINGLE_BLOCK : SD_WRITE_SINGLE_BLOCK;
		ret = sdhci_host_send_cmd(dev, &sdhci_cmd);
	}

	if (ret) {
		return ret;
	}

	if (IS_ENABLED(CONFIG_SDHCI_HOST_DMA)) {
		ret = wait_xfr_complete(dev, data->timeout_ms);
	} else {
		if (read) {
			ret = read_data_port(dev, data);
		} else {
			ret = write_data_port(dev, data);
		}
	}

	if (!IS_ENABLED(CONFIG_SDHCI_HOST_AUTO_STOP)) {
		sdhci_stop_transfer(dev);
	}
	return ret;
}

static int sdhci_request(const struct device *dev, struct sdhc_command *cmd, struct sdhc_data *data)
{
	int ret;

	LOG_DBG("");

	if (data) {
		switch (cmd->opcode) {
		case SD_WRITE_SINGLE_BLOCK:
		case SD_WRITE_MULTIPLE_BLOCK:
			LOG_DBG("SD_WRITE_SINGLE_BLOCK");
			ret = sdhci_xfr(dev, cmd, data, false);
			break;

		case SD_READ_SINGLE_BLOCK:
		case SD_READ_MULTIPLE_BLOCK:
			LOG_DBG("SD_READ_SINGLE_BLOCK");
			ret = sdhci_xfr(dev, cmd, data, true);
			break;

		case MMC_SEND_EXT_CSD:
			LOG_DBG("SDHCI_HOST_SEND_EXT_CSD");
			ret = sdhci_send_cmd_data(dev, MMC_SEND_EXT_CSD, cmd, data, true);
			break;

		default:
			ret = sdhci_send_cmd_data(dev, cmd->opcode, cmd, data, true);
		}
	} else {
		ret = sdhci_send_cmd_no_data(dev, cmd->opcode, cmd);
	}

	return ret;
}

static int sdhci_set_io(const struct device *dev, struct sdhc_io *ios)
{
	struct sdhci_data *sdhci = dev->data;
	struct sdhc_io *host_io = &sdhci->host_io;
	int ret;

	LOG_DBG("sdhci I/O: DW %d, Clk %d Hz, card power state %s, voltage %s", ios->bus_width,
		ios->clock, ios->power_mode == SDHC_POWER_ON ? "ON" : "OFF",
		ios->signal_voltage == SD_VOL_1_8_V ? "1.8V" : "3.3V");

	if (ios->clock && (ios->clock > sdhci->props.f_max || ios->clock < sdhci->props.f_min)) {
		LOG_ERR("Invalid argument for clock freq: %d Support max:%d and Min:%d", ios->clock,
			sdhci->props.f_max, sdhci->props.f_min);
		return -EINVAL;
	}

	/* Set HC clock */
	if (host_io->clock != ios->clock) {
		LOG_DBG("Clock: %d", host_io->clock);
		if (ios->clock != 0) {
			/* Enable clock */
			LOG_DBG("CLOCK: %d", ios->clock);
			if (sdhci_set_clock(dev, ios->clock) < 0) {
				return -ENOTSUP;
			}
		} else {
			ret = sdhci_clock_ctrl(dev, false);
			if (ret < 0) {
				return ret;
			}
		}
		host_io->clock = ios->clock;
	}

	/* Set data width */
	if (host_io->bus_width != ios->bus_width) {
		LOG_DBG("bus_width: %d", host_io->bus_width);

		/*
		 * SDHCI_HOST_CTRL_EDTW / SDHCI_HOST_CTRL_DTW are single-bit
		 * flags (unlike the old multi-bit SET_BITS fields). The
		 * branch structure below is kept identical to the original
		 * driver logic.
		 */
		if (ios->bus_width == SDHC_BUS_WIDTH4BIT) {
			if (ios->bus_width == SDHC_BUS_WIDTH8BIT) {
				SDHCI_SET_BITS(dev, SDHCI_HOST_CTRL, SDHCI_HOST_CTRL_EDTW);
			} else {
				SDHCI_CLEAR_BITS(dev, SDHCI_HOST_CTRL, SDHCI_HOST_CTRL_EDTW);
			}
		} else {
			if (ios->bus_width == SDHC_BUS_WIDTH4BIT) {
				SDHCI_SET_BITS(dev, SDHCI_HOST_CTRL, SDHCI_HOST_CTRL_DTW);
			} else {
				SDHCI_CLEAR_BITS(dev, SDHCI_HOST_CTRL, SDHCI_HOST_CTRL_DTW);
			}
		}
		host_io->bus_width = ios->bus_width;
	}

	/* Set HC signal voltage */
	if (ios->signal_voltage != host_io->signal_voltage) {
		LOG_DBG("signal_voltage: %d", ios->signal_voltage);
		ret = sdhci_set_voltage(dev, ios->signal_voltage);
		if (ret) {
			LOG_ERR("Set signal voltage failed:%d", ret);
			return ret;
		}
		host_io->signal_voltage = ios->signal_voltage;
	}

	/* Set card power */
	if (host_io->power_mode != ios->power_mode) {
		LOG_DBG("power_mode: %d", ios->power_mode);

		sdhci_set_power(dev, ios->power_mode);
		host_io->power_mode = ios->power_mode;
	}

	/* Set I/O timing */
	if (host_io->timing != ios->timing) {
		LOG_DBG("timing: %d", ios->timing);

		ret = sdhci_set_timing(dev, ios->timing);
		if (ret) {
			LOG_ERR("Set timing failed:%d", ret);
			return ret;
		}
		host_io->timing = ios->timing;
	}

	return 0;
}

static int sdhci_get_card_present(const struct device *dev)
{
	struct sdhci_data *sdhci = dev->data;

	LOG_DBG("");

	sdhci->card_present =
		(bool)(SDHCI_REG_READ(dev, SDHCI_PRESENT_STATE) & SDHCI_PRESENT_STATE_CI);

	if (!sdhci->card_present) {
		LOG_ERR("No MMC device detected");
	}

	return ((int)sdhci->card_present);
}

static int sdhci_card_busy(const struct device *dev)
{
	LOG_DBG("");

	if (SDHCI_REG_READ(dev, SDHCI_PRESENT_STATE) &
	    (SDHCI_PRESENT_STATE_CICMD | SDHCI_PRESENT_STATE_CIDAT | SDHCI_PRESENT_STATE_DLA)) {
		return 1;
	}

	return 0;
}

static int sdhci_get_host_props(const struct device *dev, struct sdhc_host_props *props)
{
	struct sdhci_data *sdhci = dev->data;
	const struct sdhci_config *config = dev->config;
	uint32_t caps1 = SDHCI_REG_READ(dev, SDHCI_CAPS1);
	uint32_t caps2 = SDHCI_REG_READ(dev, SDHCI_CAPS2);

	LOG_DBG("");

	memset(props, 0, sizeof(struct sdhc_host_props));
	props->f_max = config->max_bus_freq;
	props->f_min = config->min_bus_freq;
	props->power_delay = config->power_delay_ms;

	props->host_caps.vol_180_support = (bool)(caps1 & SDHCI_CAPS1_VS18);
	props->host_caps.vol_300_support = (bool)(caps1 & SDHCI_CAPS1_VS30);
	props->host_caps.vol_330_support = (bool)(caps1 & SDHCI_CAPS1_VS33);
	props->host_caps.suspend_res_support = false;
	props->host_caps.sdma_support = (bool)(caps1 & SDHCI_CAPS1_DMAS);
	props->host_caps.high_spd_support = (bool)(caps1 & SDHCI_CAPS1_HSS);
	props->host_caps.adma_2_support = (bool)(caps1 & SDHCI_CAPS1_ADMA2S);

	props->host_caps.max_blk_len = (caps1 & SDHCI_CAPS1_MBL_MASK) >> SDHCI_CAPS1_MBL_POS;
	props->host_caps.ddr50_support = (bool)(caps2 & SDHCI_CAPS2_DDR50);
	props->host_caps.sdr104_support = (bool)(caps2 & SDHCI_CAPS2_SDR104);
	props->host_caps.sdr50_support = (bool)(caps2 & SDHCI_CAPS2_SDR50);
	props->host_caps.bus_8_bit_support = true;
	props->bus_4_bit_support = true;
	props->hs200_support = (bool)config->hs200_mode;
	props->hs400_support = (bool)config->hs400_mode;

	sdhci->props = *props;

	return 0;
}

static void sdhci_isr(const struct device *dev)
{
	struct sdhci_data *sdhci = dev->data;
	uint32_t int_stat = SDHCI_REG_READ(dev, SDHCI_INT_STATUS);
	uint32_t adma_err;
	uint32_t w1c = 0;

	if (int_stat & SDHCI_INT_STATUS_CC) {
		w1c |= SDHCI_INT_STATUS_CC;
		k_event_post(&sdhci->irq_event, SDHCI_INT_STATUS_CC);
	}

	if (int_stat & SDHCI_INT_STATUS_TC) {
		w1c |= SDHCI_INT_STATUS_TC;
		k_event_post(&sdhci->irq_event, SDHCI_INT_STATUS_TC);
	}

	if (int_stat & SDHCI_INT_STATUS_DMAINT) {
		w1c |= SDHCI_INT_STATUS_DMAINT;
		k_event_post(&sdhci->irq_event, SDHCI_INT_STATUS_DMAINT);
	}

	if (int_stat & SDHCI_INT_STATUS_BWR) {
		w1c |= SDHCI_INT_STATUS_BWR;
		k_event_post(&sdhci->irq_event, SDHCI_INT_STATUS_BWR);
	}

	if (int_stat & SDHCI_INT_STATUS_BRR) {
		w1c |= SDHCI_INT_STATUS_BRR;
		k_event_post(&sdhci->irq_event, SDHCI_INT_STATUS_BRR);
	}

	if (int_stat & SDHCI_ERROR_INTERRUPT_MASK) {
		LOG_ERR("err int:%x", int_stat & SDHCI_ERROR_INTERRUPT_MASK);
		k_event_post(&sdhci->irq_event,
			     ERROR_INTERRUPT_STATUS_EVENT(int_stat & SDHCI_ERROR_INTERRUPT_MASK));
		if (int_stat & SDHCI_INT_STATUS_EADMA) {
			w1c |= SDHCI_INT_STATUS_EADMA;
		} else {
			w1c |= (int_stat & SDHCI_ERROR_INTERRUPT_MASK);
		}
	}

	if (int_stat) {
		k_event_post(&sdhci->irq_event, int_stat);
		w1c |= int_stat;
	}

	if (w1c) {
		SDHCI_REG_WRITE(dev, SDHCI_INT_STATUS, w1c);
	}

	adma_err = SDHCI_REG_READ(dev, SDHCI_ADMA_ERROR);
	if (adma_err) {
		LOG_ERR("adma err:%x", adma_err);
	}
}

static int sdhci_init(const struct device *dev)
{
	struct sdhci_data *sdhci = dev->data;
	const struct sdhci_config *config = dev->config;

	k_sem_init(&sdhci->lock, 1, 1);
	k_event_init(&sdhci->irq_event);

#if DT_ANY_INST_ON_BUS_STATUS_OKAY(pcie)
	if (config->pcie) {
		struct pcie_bar mbar;

		if (config->pcie->bdf == PCIE_BDF_NONE) {
			LOG_ERR("Cannot probe eMMC PCI device: %x", config->pcie->id);
			return -ENODEV;
		}

		if (!pcie_probe_mbar(config->pcie->bdf, 0, &mbar)) {
			LOG_ERR("eMMC MBAR not found");
			return -EINVAL;
		}

		pcie_get_mbar(config->pcie->bdf, 0, &mbar);
		pcie_set_cmd(config->pcie->bdf, PCIE_CONF_CMDSTAT_MEM, true);
		device_map(DEVICE_MMIO_RAM_PTR(dev), mbar.phys_addr, mbar.size, K_MEM_CACHE_NONE);
		pcie_set_cmd(config->pcie->bdf, PCIE_CONF_CMDSTAT_MASTER, true);
	} else
#endif
	{
		DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);
	}

	LOG_DBG("MMC Device MMIO: %lx", (unsigned long)DEVICE_MMIO_GET(dev));

	if (IS_ENABLED(CONFIG_SDHCI_HOST_INTERRUPT)) {
		config->config_func(dev);
	}

	return sdhci_reset(dev);
}

static DEVICE_API(sdhc, sdhci_api) = {
	.reset = sdhci_reset,
	.request = sdhci_request,
	.set_io = sdhci_set_io,
	.get_card_present = sdhci_get_card_present,
	.card_busy = sdhci_card_busy,
	.get_host_props = sdhci_get_host_props,
};

#define SDHCI_HOST_IRQ_FLAGS_SENSE0(n) 0
#define SDHCI_HOST_IRQ_FLAGS_SENSE1(n) DT_INST_IRQ(n, sense)
#define SDHCI_HOST_IRQ_FLAGS(n)                                                                    \
	_CONCAT(SDHCI_HOST_IRQ_FLAGS_SENSE, DT_INST_IRQ_HAS_CELL(n, sense))(n)

/* Not PCI(e) */
#define SDHCI_HOST_IRQ_CONFIG_PCIE0(n)                                                             \
	static void sdhci_config_##n(const struct device *port)                                    \
	{                                                                                          \
		ARG_UNUSED(port);                                                                  \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), sdhci_isr,                  \
			    DEVICE_DT_INST_GET(n), SDHCI_HOST_IRQ_FLAGS(n));                       \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}

/* PCI(e) with auto IRQ detection */
#define SDHCI_HOST_IRQ_CONFIG_PCIE1(n)                                                             \
	static void sdhci_config_##n(const struct device *port)                                    \
	{                                                                                          \
		BUILD_ASSERT(DT_INST_IRQN(n) == PCIE_IRQ_DETECT,                                   \
			     "Only runtime IRQ configuration is supported");                       \
		BUILD_ASSERT(IS_ENABLED(CONFIG_DYNAMIC_INTERRUPTS),                                \
			     "eMMC PCI device needs CONFIG_DYNAMIC_INTERRUPTS");                   \
		const struct sdhci_config *const dev_cfg = port->config;                           \
		unsigned int irq = pcie_alloc_irq(dev_cfg->pcie->bdf);                             \
                                                                                                   \
		if (irq == PCIE_CONF_INTERRUPT_IRQ_NONE) {                                         \
			return;                                                                    \
		}                                                                                  \
		pcie_connect_dynamic_irq(dev_cfg->pcie->bdf, irq, DT_INST_IRQ(n, priority),        \
					 (void (*)(const void *))sdhci_isr, DEVICE_DT_INST_GET(n), \
					 SDHCI_HOST_IRQ_FLAGS(n));                                 \
		pcie_irq_enable(dev_cfg->pcie->bdf, irq);                                          \
	}

#define SDHCI_HOST_IRQ_CONFIG(n) _CONCAT(SDHCI_HOST_IRQ_CONFIG_PCIE, DT_INST_ON_BUS(n, pcie))(n)

#define INIT_PCIE0(n)
#define INIT_PCIE1(n) DEVICE_PCIE_INST_INIT(n, pcie),
#define INIT_PCIE(n)  _CONCAT(INIT_PCIE, DT_INST_ON_BUS(n, pcie))(n)

#define REG_INIT_PCIE0(n) DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)),
#define REG_INIT_PCIE1(n)
#define REG_INIT(n) _CONCAT(REG_INIT_PCIE, DT_INST_ON_BUS(n, pcie))(n)

#define DEFINE_PCIE0(n)
#define DEFINE_PCIE1(n)           DEVICE_PCIE_INST_DECLARE(n)
#define SDHCI_HOST_PCIE_DEFINE(n) _CONCAT(DEFINE_PCIE, DT_INST_ON_BUS(n, pcie))(n)

#define SDHCI_HOST_DEV_CFG(n)                                                                      \
	SDHCI_HOST_PCIE_DEFINE(n);                                                                 \
	SDHCI_HOST_IRQ_CONFIG(n);                                                                  \
	static const struct sdhci_config sdhci_config_data_##n = {                                 \
		REG_INIT(n)                                                                        \
		INIT_PCIE(n).config_func = sdhci_config_##n,                                       \
		.hs200_mode = DT_INST_PROP_OR(n, mmc_hs200_1_8v, 0),                               \
		.hs400_mode = DT_INST_PROP_OR(n, mmc_hs400_1_8v, 0),                               \
		.dw_4bit = DT_INST_ENUM_HAS_VALUE(n, bus_width, 4),                                \
		.dw_8bit = DT_INST_ENUM_HAS_VALUE(n, bus_width, 8),                                \
		.max_bus_freq = DT_INST_PROP(n, max_bus_freq),                                     \
		.min_bus_freq = DT_INST_PROP(n, min_bus_freq),                                     \
		.power_delay_ms = DT_INST_PROP_OR(n, power_delay_ms, 500),                         \
	};                                                                                         \
                                                                                                   \
	static struct sdhci_data sdhci_priv_data_##n;                                              \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, sdhci_init, NULL, &sdhci_priv_data_##n, &sdhci_config_data_##n,   \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &sdhci_api);

DT_INST_FOREACH_STATUS_OKAY(SDHCI_HOST_DEV_CFG)
