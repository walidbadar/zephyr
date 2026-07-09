/*
 * Copyright (C) 2023 Intel Corporation
 * Copyright (C) 2026 Altera Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT cdns_sdhc

#include <stdio.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/sd/sd.h>
#include <zephyr/sys/sys_io.h>

#include "sdhc_cdns.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(cdns_sdhc, CONFIG_SD_LOG_LEVEL);

#define CDNS_SDHC_SLOT_TYPE(dev)                                                                   \
	((((struct cdns_sdhc_data *)dev->data)->props.host_caps.slot_type != 0)                    \
		 ? CDNS_SDHC_EMMC_SLOT                                                             \
		 : CDNS_SDHC_SD_SLOT)

#define CDNS_SDHC_GET_HOST_PROP_BIT(cap, mask) ((uint8_t)(((cap) & (mask)) != 0U))

/* Sentinel for an invalid command frame; distinct name from any 16-bit
 * legacy sentinel in cdns_sdhc.h since the command frame is now 32-bit.
 */
#define CDNS_SDHC_CMD_FRAME_INVALID UINT32_MAX

#if defined(CONFIG_DCACHE_LINE_SIZE) && (CONFIG_DCACHE_LINE_SIZE > 0)
#define CDNS_SDHC_DESC_ALIGN CONFIG_DCACHE_LINE_SIZE
#else
#define CDNS_SDHC_DESC_ALIGN 4
#endif

#define CDNS_SDHC_DESC_MAX_LEN 	UINT32_MAX

#define DEV_CFG(dev)     ((const struct cdns_sdhc_config *)dev->config)
#define DEV_DATA(dev)    ((struct cdns_sdhc_data *)dev->data)

#if defined(CONFIG_CDNS_SDHC_ADMA2_SUPPORT)
/**
 * @brief ADMA2 descriptor table structure.
 */
typedef struct {
	/**< attrs of descriptor */
	uint16_t attr;
	/**< Length of current dma transfer max 64kb */
	uint16_t len;
	/**< source/destination addr for current dma transfer */
	uint64_t addr;
} __packed adma2_descriptor;
#endif

/**
 * @brief Holds device private data.
 */
struct cdns_sdhc_data {
	/* MMIO mapping information for SDHC software registers set */
	DEVICE_MMIO_NAMED_RAM(srs);
	/**< Current I/O settings of SDHC */
	struct sdhc_io host_io;
	/**< Supported properties of SDHC */
	struct sdhc_host_props props;
	/**< SDHC IRQ events */
	struct k_event irq_event;
	/**< transfer mode and data direction (lower 16 bits of CDNS_SDHC_XFER_MODE) */
	uint16_t transfermode;
	/**< Maximum input clock supported by HC */
	uint32_t maxclock;
	/**< Bounce buffer for small DMA reads */
	uint8_t read_bounce_buffer[CONFIG_SDHC_BUFFER_ALIGNMENT]
		__aligned(CONFIG_SDHC_BUFFER_ALIGNMENT);
#if defined(CONFIG_CDNS_SDHC_ADMA2_SUPPORT)
	/**< ADMA descriptor table */
	adma2_descriptor adma2_desc[MAX(1, CONFIG_CDNS_SDHC_DESC_SIZE)]
		__aligned(CDNS_SDHC_DESC_ALIGN);
#endif
};

/**
 * @brief Holds SDHC configuration data.
 */
struct cdns_sdhc_config {	
	/* MMIO mapping information for SDHC software registers set */
	DEVICE_MMIO_NAMED_ROM(srs);
	/**< Pointer to the device structure representing the clock bus */
	const struct device *clock_dev;
	/**< Callback to the device interrupt configuration api */
	void (*irq_config_fn)(const struct device *dev);
	/**< Card detection pin available or not */
	uint32_t freq_max;
	uint32_t freq_min;
	bool broken_cd;
	/**< Support hs200 mode. */
	bool hs200_mode;
	/**< Support hs400 mode */
	bool hs400_mode;
	/**< delay given to card to power up or down fully */
	uint16_t powerdelay;
	/**< Used to identify HC internal phy register */
	bool has_phy;
};

/**
 * @brief
 * Read a 32-bit register at the given CDNS_SDHC_* offset.
 */
static inline uint32_t cdns_sdhc_read(const struct device *dev, uint32_t offset)
{
	return sys_read32(DEVICE_MMIO_NAMED_GET(dev, srs) + offset);
}

/**
 * @brief
 * Write a 32-bit register at the given CDNS_SDHC_* offset.
 */
static inline void cdns_sdhc_write(const struct device *dev, uint32_t offset, uint32_t value)
{
	sys_write32(value, DEVICE_MMIO_NAMED_GET(dev, srs) + offset);
}

/**
 * @brief
 * Set (OR in) bits in a 32-bit register at the given CDNS_SDHC_* offset.
 */
static inline void cdns_sdhc_set_bits(const struct device *dev, uint32_t offset, uint32_t bits)
{
	sys_set_bits(DEVICE_MMIO_NAMED_GET(dev, srs) + offset, bits);
}

/**
 * @brief
 * Clear bits in a 32-bit register at the given CDNS_SDHC_* offset.
 */
static inline void cdns_sdhc_clear_bits(const struct device *dev, uint32_t offset, uint32_t bits)
{
	sys_clear_bits(DEVICE_MMIO_NAMED_GET(dev, srs) + offset, bits);
}

/**
 * @brief
 * Polled wait for a 32-bit register (by offset) to reach `value` under `mask`.
 */
static int cdns_sdhc_wait_reg_mask(const struct device *dev, uint32_t offset, int32_t timeout_ms,
				   uint32_t mask, uint32_t value)
{
	for (uint32_t retry = 0; retry < (uint32_t)timeout_ms; retry++) {
		if ((cdns_sdhc_read(dev, offset) & mask) == value) {
			return 0;
		}
		k_msleep(1);
	}

	return -EAGAIN;
}

/**
 * @brief
 * Polled wait for any one of the given events in a 32-bit register (by offset).
 */
static int cdns_sdhc_wait_for_events(const struct device *dev, uint32_t offset, int32_t timeout_ms,
				     uint32_t events)
{
	for (uint32_t retry = 0; retry < (uint32_t)timeout_ms; retry++) {
		if ((cdns_sdhc_read(dev, offset) & events) != 0U) {
			return 0;
		}
		k_msleep(1);
	}

	return -EAGAIN;
}

/**
 * @brief
 * Check card is detected by host
 */
static int cdns_sdhc_card_detect(const struct device *dev)
{
	const struct cdns_sdhc_config *config = dev->config;
	uint32_t present_state = cdns_sdhc_read(dev, CDNS_SDHC_PRESENT_STATE);

	if ((present_state & CDNS_SDHC_PRESENT_STATE_CI) != 0U) {
		return 1;
	}

	/* In case of polling always treat card is detected */
	if (config->broken_cd == true) {
		return 1;
	}

	return 0;
}

/**
 * @brief
 * Clear the controller status register (write-1-to-clear, normal + error bits)
 */
static void cdns_sdhc_clear_intr(const struct device *dev)
{
	cdns_sdhc_write(dev, CDNS_SDHC_INT_STATUS,
			CDNS_SDHC_NORMAL_INT_MASK | CDNS_SDHC_ERR_INT_MASK);
}

/**
 * @brief
 * Setup ADMA2 descriptor table for data transfer
 */
#if defined(CONFIG_CDNS_SDHC_ADMA2_SUPPORT)
static int cdns_sdhc_setup_dma(const struct device *dev, const struct sdhc_data *data)
{
	struct cdns_sdhc_data *sd_data = dev->data;
	const uint8_t *buff = data->data;
	const uint64_t block_chunk = data->block_size * data->blocks;
	uint64_t desc_addr;
	size_t desc_size;
	uint32_t table;
	uint32_t i;
	int ret = 0;

	if ((block_chunk) < CDNS_SDHC_DESC_MAX_LEN) {
		table = 1U;
	} else {
		table = ((block_chunk) / CDNS_SDHC_DESC_MAX_LEN);
		if (((block_chunk) % CDNS_SDHC_DESC_MAX_LEN) != 0U) {
			table += 1U;
		}
	}

	if (table > CONFIG_CDNS_SDHC_DESC_SIZE) {
		LOG_ERR("Descriptor size is too big");
		return -ENOTSUP;
	}

	for (i = 0U; i < (table - 1U); i++) {
		sd_data->adma2_desc[i].addr = ((mem_addr_t)buff + (i * CDNS_SDHC_DESC_MAX_LEN));
		sd_data->adma2_desc[i].attr =
			CDNS_SDHC_ADMA2_DESC_TRAN | CDNS_SDHC_ADMA2_DESC_VALID;
		sd_data->adma2_desc[i].len = 0U;
	}

	sd_data->adma2_desc[table - 1U].addr = ((mem_addr_t)buff + (i * CDNS_SDHC_DESC_MAX_LEN));
	sd_data->adma2_desc[table - 1U].attr =
		CDNS_SDHC_ADMA2_DESC_TRAN | CDNS_SDHC_ADMA2_DESC_END | CDNS_SDHC_ADMA2_DESC_VALID;
	sd_data->adma2_desc[table - 1U].len = ((block_chunk) - (i * CDNS_SDHC_DESC_MAX_LEN));

	/*
	 * The SDHC reads the ADMA2 descriptor table through DMA.
	 * If the table lives in cacheable memory, make sure all
	 * descriptor writes are visible to the controller before
	 * programming ADMA_SYS_ADDR.
	 */
	desc_size = table * sizeof(sd_data->adma2_desc[0]);
	ret = sys_cache_data_flush_range(sd_data->adma2_desc, desc_size);
	if (ret != 0) {
		LOG_ERR("Failed to flush ADMA descriptor table: ret=%d desc=%p size=%u", ret,
			sd_data->adma2_desc, (uint32_t)desc_size);
		return ret;
	}

	desc_addr = (uint64_t)(mem_addr_t) & (sd_data->adma2_desc[0]);
	cdns_sdhc_write(dev, CDNS_SDHC_ADMA_SYS_ADDR1, (uint32_t)(desc_addr & UINT32_MAX));
	cdns_sdhc_write(dev, CDNS_SDHC_ADMA_SYS_ADDR2, (uint32_t)(desc_addr >> 32));

	return ret;
}
#else
static int cdns_sdhc_setup_dma(const struct device *dev, const struct sdhc_data *data)
{
	if (data->data > UINT32_MAX) {
		LOG_ERR("SDMA buffer addr exceeds 32-bit addrable range: %p", data->data);
		return -EINVAL;
	}

	cdns_sdhc_write(dev, CDNS_SDHC_SDMA_ADDR, (uint32_t)data->data);

	return 0;
}
#endif

/**
 * @brief
 * Frame the command into the 32-bit CDNS_SDHC_XFER_MODE upper half (Command
 * register bits). Response-type -> Response Type Select / index / CRC
 * check-enable encoding follows the standard CDNS_SDHC Simplified Spec table.
 */
static uint32_t cdns_sdhc_cmd_frame(struct sdhc_command *cmd, bool data, uint8_t slottype)
{
	uint32_t command =
		(cmd->opcode << CDNS_SDHC_XFER_MODE_CIDX_POS) & CDNS_SDHC_XFER_MODE_CIDX_MASK;

	switch (cmd->response_type & SDHC_NATIVE_RESPONSE_MASK) {
	case SD_RSP_TYPE_NONE:
		command |= CDNS_RESP_NONE;
		break;

	case SD_RSP_TYPE_R1:
		command |= CDNS_RESP_R1;
		break;

	case SD_RSP_TYPE_R1b:
		command |= CDNS_RESP_R1B;
		break;

	case SD_RSP_TYPE_R2:
		command |= CDNS_RESP_R2;
		break;

	case SD_RSP_TYPE_R3:
		command |= CDNS_RESP_R3;
		break;

	case SD_RSP_TYPE_R4:
		command |= CDNS_RESP_R3;
		break;

	case SD_RSP_TYPE_R5:
		command |= CDNS_RESP_R1;
		break;

	case SD_RSP_TYPE_R6:
		command |= CDNS_RESP_R6;
		break;

	case SD_RSP_TYPE_R7:
		/* As per spec, EMMC does not support R7 */
		if (slottype == CDNS_SDHC_EMMC_SLOT) {
			return CDNS_SDHC_CMD_FRAME_INVALID;
		}
		command |= CDNS_RESP_R1;
		break;

	default:
		LOG_DBG("Invalid response type");
		return CDNS_SDHC_CMD_FRAME_INVALID;
	}

	/* EMMC does not support APP command */
	if ((cmd->opcode == SD_APP_CMD) && (slottype == CDNS_SDHC_EMMC_SLOT)) {
		LOG_DBG("Invalid response type");
		return CDNS_SDHC_CMD_FRAME_INVALID;
	}

	if (data) {
		command |= CDNS_SDHC_XFER_MODE_DPS;
	}

	return command;
}

/**
 * @brief
 * Check command response is success or failed also clears status register
 */
static int cdns_sdhc_cmd_response(const struct device *dev, struct sdhc_command *cmd)
{
	const struct cdns_sdhc_config *config = dev->config;
	struct cdns_sdhc_data *sd_data = dev->data;
	uint32_t events;
	uint32_t status;
	uint32_t mask;
	int ret;
	k_timeout_t timeout;

	mask = CDNS_SDHC_INT_STATUS_EINT | CDNS_SDHC_INT_STATUS_CC;
	if ((cmd->opcode == SD_SEND_TUNING_BLOCK) || (cmd->opcode == MMC_SEND_TUNING_BLOCK)) {
		mask |= CDNS_SDHC_INT_STATUS_BRR;
	}

	if (config->irq_config_fn == NULL) {
		ret = cdns_sdhc_wait_for_events(dev, CDNS_SDHC_INT_STATUS, cmd->timeout_ms, mask);
		if (ret != 0) {
			LOG_ERR("No response from card");
			return ret;
		}

		status = cdns_sdhc_read(dev, CDNS_SDHC_INT_STATUS);
		if ((status & CDNS_SDHC_INT_STATUS_EINT) != 0U) {
			LOG_ERR("Error response from card");
			cdns_sdhc_write(dev, CDNS_SDHC_INT_STATUS, CDNS_SDHC_ERR_INT_MASK);
			return -EINVAL;
		}
		cdns_sdhc_write(dev, CDNS_SDHC_INT_STATUS, CDNS_SDHC_INT_STATUS_CC);
	} else {
		timeout = K_MSEC(cmd->timeout_ms);

		events = k_event_wait(&sd_data->irq_event, mask, false, timeout);

		if ((events & CDNS_SDHC_INT_STATUS_EINT) != 0U) {
			LOG_ERR("Error response from card");
			ret = -EINVAL;
		} else if ((events & (CDNS_SDHC_INT_STATUS_CC | CDNS_SDHC_INT_STATUS_BRR)) != 0U) {
			ret = 0;
		} else {
			LOG_ERR("No response from card");
			ret = -EAGAIN;
		}
	}
	return ret;
}

/**
 * @brief
 * Update response member of command structure which is used by subsystem
 */
static void cdns_sdhc_update_response(const struct device *dev, struct sdhc_command *cmd)
{
	if (cmd->response_type == SD_RSP_TYPE_NONE) {
		return;
	}

	if (cmd->response_type == SD_RSP_TYPE_R2) {
		cmd->response[0] = cdns_sdhc_read(dev, CDNS_SDHC_RESPONSE0);
		cmd->response[1] = cdns_sdhc_read(dev, CDNS_SDHC_RESPONSE1);
		cmd->response[2] = cdns_sdhc_read(dev, CDNS_SDHC_RESPONSE2);
		cmd->response[3] = cdns_sdhc_read(dev, CDNS_SDHC_RESPONSE3);

		/* CRC is striped from the response performing shifting to update response */
		for (uint8_t i = 3; i != 0; i--) {
			cmd->response[i] <<= CDNS_SDHC_CRC_LEFT_SHIFT;
			cmd->response[i] |= cmd->response[i - 1] >> CDNS_SDHC_CRC_RIGHT_SHIFT;
		}
		cmd->response[0] <<= CDNS_SDHC_CRC_LEFT_SHIFT;
	} else {
		cmd->response[0] = cdns_sdhc_read(dev, CDNS_SDHC_RESPONSE0);
	}
}

/**
 * @brief
 * Setup and send the command and also check for response
 */
static int cdns_sdhc_cmd(const struct device *dev, struct sdhc_command *cmd, bool data)
{
	const struct cdns_sdhc_config *config = dev->config;
	struct cdns_sdhc_data *sd_data = dev->data;
	uint8_t slottype = CDNS_SDHC_SLOT_TYPE(dev);
	uint32_t command;
	int ret;

	cdns_sdhc_write(dev, CDNS_SDHC_ARGUMENT, cmd->arg);

	cdns_sdhc_clear_intr(dev);

	/* Frame command */
	command = cdns_sdhc_cmd_frame(cmd, data, slottype);
	if (command == CDNS_SDHC_CMD_FRAME_INVALID) {
		return -EINVAL;
	}

	if ((cmd->opcode != SD_SEND_TUNING_BLOCK) && (cmd->opcode != MMC_SEND_TUNING_BLOCK)) {
		uint32_t present_state = cdns_sdhc_read(dev, CDNS_SDHC_PRESENT_STATE);

		if (((present_state & CDNS_SDHC_PRESENT_STATE_CIDAT) != 0U) &&
		    ((command & CDNS_SDHC_XFER_MODE_DPS) != 0U)) {
			LOG_ERR("Card data lines busy");
			return -EBUSY;
		}
	}

	if (config->irq_config_fn != NULL) {
		k_event_clear(&sd_data->irq_event, CDNS_SDHC_TXFR_INTR_EN_MASK);
	}

	/* SRS03: Transfer Mode occupies bits 15:0, Command occupies bits 31:16 */
	cdns_sdhc_write(dev, CDNS_SDHC_XFER_MODE, command | sd_data->transfermode);

	/* Check for response */
	ret = cdns_sdhc_cmd_response(dev, cmd);
	if (ret != 0) {
		return ret;
	}

	cdns_sdhc_update_response(dev, cmd);

	return 0;
}

/**
 * @brief
 * Check for data transfer completion
 */
static int cdns_sdhc_xfr(const struct device *dev, struct sdhc_data *data)
{
	const struct cdns_sdhc_config *config = dev->config;
	struct cdns_sdhc_data *sd_data = dev->data;
	k_timeout_t timeout;
	uint32_t events;
	uint32_t mask;
	uint32_t status;
	int ret;

	mask = CDNS_SDHC_ERR_INT_MASK | CDNS_SDHC_INT_STATUS_TC;
	if (config->irq_config_fn == NULL) {
		ret = cdns_sdhc_wait_for_events(dev, CDNS_SDHC_INT_STATUS, data->timeout_ms, mask);
		if (ret != 0) {
			LOG_ERR("Data transfer timeout");
			return ret;
		}

		status = cdns_sdhc_read(dev, CDNS_SDHC_INT_STATUS);
		if ((status & CDNS_SDHC_ERR_INT_MASK) != 0U) {
			cdns_sdhc_write(dev, CDNS_SDHC_INT_STATUS, CDNS_SDHC_ERR_INT_MASK);
			LOG_ERR("Error at data transfer");
			return -EINVAL;
		}

		cdns_sdhc_write(dev, CDNS_SDHC_INT_STATUS, CDNS_SDHC_INT_STATUS_TC);
	} else {
		timeout = K_MSEC(data->timeout_ms);

		events = k_event_wait(&sd_data->irq_event, mask, false, timeout);

		if ((events & CDNS_SDHC_ERR_INT_MASK) != 0U) {
			LOG_ERR("Error at data transfer");
			ret = -EINVAL;
		} else if ((events & CDNS_SDHC_INT_STATUS_TC) != 0U) {
			ret = 0;
		} else {
			LOG_ERR("Data transfer timeout");
			ret = -EAGAIN;
		}
	}

	return ret;
}

/**
 * @brief
 * Performs data and command transfer and check for transfer complete
 */
static int cdns_sdhc_transfer(const struct device *dev, struct sdhc_command *cmd,
			       struct sdhc_data *data)
{
	struct cdns_sdhc_data *sd_data = dev->data;
	struct sdhc_data *dma_data = data;
	struct sdhc_data bounce_data;
	uint64_t block_chunk;
	uint64_t dma_len;
	uint32_t block_reg;
	size_t cache_line_size;
	bool bounced = false;
	bool read;
	int ret;

	/* Check command line is in use */
	if ((cdns_sdhc_read(dev, CDNS_SDHC_PRESENT_STATE) & CDNS_SDHC_PRESENT_STATE_CICMD) != 0U) {
		LOG_ERR("Command lines are busy");
		return -EBUSY;
	}

	if (data == NULL) {
		/* Send command and check for command complete */
		return cdns_sdhc_cmd(dev, cmd, false);
	}

	block_chunk = (uint64_t)data->block_size * data->blocks;
	dma_len = block_chunk;

	block_reg = ((data->blocks << CDNS_SDHC_BLOCK_SIZE_BCCT_POS) &
		     CDNS_SDHC_BLOCK_SIZE_BCCT_MASK) |
		    data->block_size;
	cdns_sdhc_write(dev, CDNS_SDHC_BLOCK_SIZE, block_reg);

	read = (sd_data->transfermode & CDNS_SDHC_XFER_MODE_DTDS) != 0U;
	cache_line_size = sys_cache_data_line_size_get();

	if (read && (cache_line_size != 0U)) {
		if (!IS_ALIGNED((mem_addr_t)data->data, cache_line_size)) {
			LOG_ERR("Read DMA buffer must be aligned to d-cache line size: "
				"buf=%p line_size=%u",
				data->data, (uint32_t)cache_line_size);
			return -EINVAL;
		}

		/*
		 * Cache invalidation requires a cache-line-sized range. Use a
		 * dedicated bounce buffer when the transfer length isn't a
		 * multiple of the cache line size.
		 */
		if (!IS_ALIGNED(block_chunk, cache_line_size)) {
			dma_len = ROUND_UP(block_chunk, cache_line_size);
			if ((dma_len > sizeof(sd_data->read_bounce_buffer)) ||
			    !IS_ALIGNED((mem_addr_t)sd_data->read_bounce_buffer,
					cache_line_size)) {
				LOG_ERR("Read DMA bounce buffer is incompatible: "
					"len=%u line_size=%u bounce_size=%u",
					(uint32_t)dma_len, (uint32_t)cache_line_size,
					(uint32_t)sizeof(sd_data->read_bounce_buffer));
				return -EINVAL;
			}

			bounce_data = *data;
			bounce_data.data = sd_data->read_bounce_buffer;
			dma_data = &bounce_data;
			bounced = true;
		}
	}

	/*
	 * For read transfers, clean and invalidate the DMA destination before
	 * starting DMA, then invalidate it again before the CPU reads data
	 * written by SDHC. For write transfers, flush CPU-written data
	 * before SDHC reads it.
	 */
	ret = sys_cache_data_flush_range(dma_data->data, dma_len);
	if ((ret == 0) && read) {
		ret = sys_cache_data_invd_range(dma_data->data, dma_len);
	}
	if (ret != 0) {
		LOG_ERR("DMA buffer cache maintenance failed before transfer: "
			"ret=%d buf=%p len=%u read=%u",
			ret, dma_data->data, (uint32_t)dma_len, read);
		return ret;
	}

	/* Setup DMA if data is present */
	ret = cdns_sdhc_setup_dma(dev, dma_data);
	if (ret != 0) {
		return ret;
	}

	/* Send command and check for command complete */
	ret = cdns_sdhc_cmd(dev, cmd, true);
	if (ret != 0) {
		return ret;
	}

	/* Check for data transfer complete */
	ret = cdns_sdhc_xfr(dev, data);
	if (ret != 0) {
		return ret;
	}

	if (read) {
		ret = sys_cache_data_invd_range(dma_data->data, dma_len);
		if (ret != 0) {
			return ret;
		}

		if (bounced) {
			memcpy(data->data, sd_data->read_bounce_buffer, block_chunk);
		}
	}

	return 0;
}

/**
 * @brief
 * Configure transfer mode and transfer command and data
 */
static int cdns_sdhc_request(const struct device *dev, struct sdhc_command *cmd,
			     struct sdhc_data *data)
{
	struct cdns_sdhc_data *sd_data = dev->data;
	int ret;

	if (sd_data->transfermode == 0U) {
		sd_data->transfermode = CDNS_SDHC_XFER_MODE_BCE | CDNS_SDHC_XFER_MODE_DTDS |
					CDNS_SDHC_XFER_MODE_DMAE;
	}

	switch (cmd->opcode) {
	case SD_READ_MULTIPLE_BLOCK:
		sd_data->transfermode |= CDNS_SDHC_XFER_MODE_CMD12_EN | CDNS_SDHC_XFER_MODE_MSBS;
		ret = cdns_sdhc_transfer(dev, cmd, data);
		break;

	case SD_WRITE_MULTIPLE_BLOCK:
		sd_data->transfermode |= CDNS_SDHC_XFER_MODE_CMD12_EN | CDNS_SDHC_XFER_MODE_MSBS;
		sd_data->transfermode &= ~CDNS_SDHC_XFER_MODE_DTDS;
		ret = cdns_sdhc_transfer(dev, cmd, data);
		break;

	case SD_WRITE_SINGLE_BLOCK:
		sd_data->transfermode &= ~CDNS_SDHC_XFER_MODE_DTDS;
		ret = cdns_sdhc_transfer(dev, cmd, data);
		break;

	case SDIO_RW_EXTENDED:
		if (IS_BIT_SET(cmd->arg, SDIO_CMD_ARG_RW_SHIFT)) {
			sd_data->transfermode &= ~CDNS_SDHC_XFER_MODE_DTDS;
		}
		if (data->blocks > 1) {
			sd_data->transfermode |= CDNS_SDHC_XFER_MODE_MSBS;
		}
		ret = cdns_sdhc_transfer(dev, cmd, data);
		break;

	case SDIO_RW_DIRECT:
		if (IS_BIT_SET(cmd->arg, SDIO_CMD_ARG_RW_SHIFT)) {
			sd_data->transfermode &= ~CDNS_SDHC_XFER_MODE_DTDS;
		}
		ret = cdns_sdhc_transfer(dev, cmd, data);
		break;

	default:
		ret = cdns_sdhc_transfer(dev, cmd, data);
	}
	sd_data->transfermode = 0;

	return ret;
}

/**
 * @brief
 * Populate sdhc_host_props structure with all sd host controller property
 */
static int cdns_sdhc_host_props(const struct device *dev, struct sdhc_host_props *props)
{
	const struct cdns_sdhc_config *config = dev->config;
	struct cdns_sdhc_data *sd_data = dev->data;
	struct sdhc_host_caps *host_caps = &props->host_caps;
	uint32_t caps1 = cdns_sdhc_read(dev, CDNS_SDHC_CAPS1);
	uint32_t caps2 = cdns_sdhc_read(dev, CDNS_SDHC_CAPS2);
	uint32_t current1 = cdns_sdhc_read(dev, CDNS_SDHC_MAX_CURRENT1);

	props->f_max = config->freq_max;
	props->f_min = config->freq_min;
	props->power_delay = config->powerdelay;

	/* Single-bit capability flags */
	host_caps->vol_180_support = CDNS_SDHC_GET_HOST_PROP_BIT(caps1, CDNS_SDHC_CAPS1_VS18);
	host_caps->vol_300_support = CDNS_SDHC_GET_HOST_PROP_BIT(caps1, CDNS_SDHC_CAPS1_VS30);
	host_caps->vol_330_support = CDNS_SDHC_GET_HOST_PROP_BIT(caps1, CDNS_SDHC_CAPS1_VS33);
	host_caps->sdma_support = CDNS_SDHC_GET_HOST_PROP_BIT(caps1, CDNS_SDHC_CAPS1_DMAS);
	host_caps->high_spd_support = CDNS_SDHC_GET_HOST_PROP_BIT(caps1, CDNS_SDHC_CAPS1_HSS);
	host_caps->adma_2_support = CDNS_SDHC_GET_HOST_PROP_BIT(caps1, CDNS_SDHC_CAPS1_ADMA2S);
	host_caps->bus_8_bit_support = CDNS_SDHC_GET_HOST_PROP_BIT(caps1, CDNS_SDHC_CAPS1_EDS8);
	host_caps->ddr50_support = CDNS_SDHC_GET_HOST_PROP_BIT(caps2, CDNS_SDHC_CAPS2_DDR50);
	host_caps->sdr104_support = CDNS_SDHC_GET_HOST_PROP_BIT(caps2, CDNS_SDHC_CAPS2_SDR104);
	host_caps->sdr50_support = CDNS_SDHC_GET_HOST_PROP_BIT(caps2, CDNS_SDHC_CAPS2_SDR50);

	/* Multi-bit masked fields — use MASK/POS, not CDNS_SDHC_GET_HOST_PROP_BIT */
	props->max_current_330 = (uint8_t)((current1 & CDNS_SDHC_MAX_CURRENT1_MC33_MASK) >>
					   CDNS_SDHC_MAX_CURRENT1_MC33_POS);
	props->max_current_300 = (uint8_t)((current1 & CDNS_SDHC_MAX_CURRENT1_MC30_MASK) >>
					   CDNS_SDHC_MAX_CURRENT1_MC30_POS);
	props->max_current_180 = (uint8_t)((current1 & CDNS_SDHC_MAX_CURRENT1_MC18_MASK) >>
					   CDNS_SDHC_MAX_CURRENT1_MC18_POS);
	host_caps->max_blk_len =
		(uint8_t)((caps1 & CDNS_SDHC_CAPS1_MBL_MASK) >> CDNS_SDHC_CAPS1_MBL_POS);
	host_caps->slot_type =
		(uint8_t)((caps1 & CDNS_SDHC_CAPS1_SLT_MASK) >> CDNS_SDHC_CAPS1_SLT_POS);

	/*
	 * The standard CDNS_SDHC capability register set has no dedicated
	 * "4-bit support" bit (4-bit mode is assumed always available),
	 * matching the previous driver's behavior.
	 */
	props->bus_4_bit_support = 1U;

	if (config->hs400_mode && config->has_phy) {
		props->hs400_support = true;
	}
	props->hs200_support = config->hs200_mode;

	sd_data->props = *props;

	return 0;
}

/**
 * @brief
 * Calculate clock value based on the selected speed
 */
static uint16_t cdns_sdhc_calc_clock(uint32_t maxclock, enum sdhc_clock_speed speed)
{
	uint16_t divcnt;
	uint16_t divisor = 0U, clockval = 0U;

	if (maxclock <= speed) {
		divisor = 0U;
	} else {
		for (divcnt = 2U; divcnt <= 0x7FEU; divcnt += 2U) {
			if ((maxclock / divcnt) <= speed) {
				divisor = divcnt >> 1U;
				break;
			}
		}
	}

	clockval |= (divisor & UINT8_MAX) << CDNS_SDHC_CLOCK_CTRL_SDCFSL_POS;
	clockval |= ((divisor >> CDNS_SDHC_CLOCK_CTRL_SDCFSL_POS) & 3U)
		    << CDNS_SDHC_CLOCK_CTRL_SDCFSH_POS;

	return clockval; /* TODO: verify cdns_sdhc_calc_clock function. */
}

/**
 * @brief
 * Set clock and wait for clock to be stable.
 *
 * CDNS_SDHC_CLOCK_CTRL is now a combined 32-bit register that also carries the
 * data-timeout value (DTCV) and the self-clearing software-reset bits.
 * Everything here is now expressed as set/clear of specific bit-fields
 * rather than an explicit read-modify-write, since none of these steps
 * ever need to combine more than one freshly-computed field into a
 * single bus transaction.
 */
static int cdns_sdhc_set_clock(const struct device *dev, enum sdhc_clock_speed speed)
{
	const struct cdns_sdhc_config *config = dev->config;
	struct cdns_sdhc_data *sd_data = dev->data;
	int ret;
	uint16_t divisor;

	/* Disable clock and clear the divisor/enable bits, keep DTCV */
	cdns_sdhc_clear_bits(dev, CDNS_SDHC_CLOCK_CTRL, (uint32_t)~CDNS_SDHC_CLOCK_CTRL_DTCV_MASK);

	if (speed == 0U) {
		return 0;
	}

	/* Get input clock rate */
	ret = clock_control_get_rate(config->clock_dev, NULL, &sd_data->maxclock);
	if (ret != 0) {
		LOG_ERR("Failed to get clock\n");
		return ret;
	}

	/* Calculate clock (already bit-positioned for the low 16 bits) */
	divisor = cdns_sdhc_calc_clock(sd_data->maxclock, speed);

	/* Program divisor and enable the internal clock */
	cdns_sdhc_set_bits(dev, CDNS_SDHC_CLOCK_CTRL, (uint32_t)divisor | CDNS_SDHC_CLOCK_CTRL_ICE);

	/* Wait max 150ms for internal clock to be stable */
	ret = cdns_sdhc_wait_reg_mask(dev, CDNS_SDHC_CLOCK_CTRL, 150, CDNS_SDHC_CLOCK_CTRL_ICS,
				      CDNS_SDHC_CLOCK_CTRL_ICS);
	if (ret != 0) {
		return ret;
	}

	/* Enable div clock */
	cdns_sdhc_set_bits(dev, CDNS_SDHC_CLOCK_CTRL, CDNS_SDHC_CLOCK_CTRL_SDCE);

	return ret;
}

/**
 * @brief
 * Set bus width on the controller (CDNS_SDHC_HOST_CTRL: DTW = 4-bit, EDTW = 8-bit)
 */
static int cdns_sdhc_set_buswidth(const struct device *dev, enum sdhc_bus_width width)
{
	switch (width) {
	case SDHC_BUS_WIDTH1BIT:
		cdns_sdhc_clear_bits(dev, CDNS_SDHC_HOST_CTRL,
				     CDNS_SDHC_HOST_CTRL_EDTW | CDNS_SDHC_HOST_CTRL_DTW);
		break;

	case SDHC_BUS_WIDTH4BIT:
		cdns_sdhc_clear_bits(dev, CDNS_SDHC_HOST_CTRL, CDNS_SDHC_HOST_CTRL_EDTW);
		cdns_sdhc_set_bits(dev, CDNS_SDHC_HOST_CTRL, CDNS_SDHC_HOST_CTRL_DTW);
		break;

	case SDHC_BUS_WIDTH8BIT:
		cdns_sdhc_set_bits(dev, CDNS_SDHC_HOST_CTRL, CDNS_SDHC_HOST_CTRL_EDTW);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

/**
 * @brief
 * Enable or disable power.
 */
static void cdns_sdhc_set_power(const struct device *dev, enum sdhc_power power)
{
	if (power == SDHC_POWER_ON) {
		cdns_sdhc_set_bits(dev, CDNS_SDHC_HOST_CTRL, CDNS_SDHC_HOST_CTRL_BP);
	} else {
		cdns_sdhc_clear_bits(dev, CDNS_SDHC_HOST_CTRL, CDNS_SDHC_HOST_CTRL_BP);
	}
}

/**
 * @brief
 * Set voltage level and signalling voltage.
 *
 * The 1.8V signalling enable bit maps to CDNS_SDHC_HOST_CTRL2_V18SE in the
 * combined host-control-2 register.
 */
static int cdns_sdhc_set_voltage(const struct device *dev, enum sd_voltage voltage)
{
	switch (voltage) {
	case SD_VOL_3_3_V:
		cdns_sdhc_set_bits(dev, CDNS_SDHC_HOST_CTRL, CDNS_SDHC_HOST_VOL_3_3_V_SELECT);
		cdns_sdhc_clear_bits(dev, CDNS_SDHC_AUTO_CMD_HOST_CTRL2,
				     CDNS_SDHC_HOST_CTRL2_V18SE);
		break;

	case SD_VOL_3_0_V:
		cdns_sdhc_set_bits(dev, CDNS_SDHC_HOST_CTRL, CDNS_SDHC_HOST_VOL_3_0_V_SELECT);
		cdns_sdhc_clear_bits(dev, CDNS_SDHC_AUTO_CMD_HOST_CTRL2,
				     CDNS_SDHC_HOST_CTRL2_V18SE);
		break;

	case SD_VOL_1_8_V:
		cdns_sdhc_set_bits(dev, CDNS_SDHC_HOST_CTRL, CDNS_SDHC_HOST_VOL_1_8_V_SELECT);
		cdns_sdhc_set_bits(dev, CDNS_SDHC_AUTO_CMD_HOST_CTRL2, CDNS_SDHC_HOST_CTRL2_V18SE);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

/**
 * @brief
 * Set speed mode and config tap delay.
 *
 * HSE lives in CDNS_SDHC_HOST_CTRL (new macro). The UHS Mode Select field
 * position/width comes from CDNS_SDHC_CTRL2_UMS_* (new macro); the
 * per-mode numeric values still come from cdns_sdhc.h
 * (CDNS_SDHC_UHS_SPEED_MODE_*) since cdns_sdhc_hw.h doesn't define them.
 */
static int cdns_sdhc_set_timing(const struct device *dev, enum sdhc_timing_mode timing)
{
	const struct cdns_sdhc_config *config = dev->config;
	uint32_t mode = 0;

	switch (timing) {
	case SDHC_TIMING_LEGACY:
		cdns_sdhc_clear_bits(dev, CDNS_SDHC_HOST_CTRL, CDNS_SDHC_HOST_CTRL_HSE);
		break;

	case SDHC_TIMING_SDR25:
	case SDHC_TIMING_HS:
		cdns_sdhc_set_bits(dev, CDNS_SDHC_HOST_CTRL, CDNS_SDHC_HOST_CTRL_HSE);
		break;

	case SDHC_TIMING_SDR12:
		mode = CDNS_SDHC_UHS_SPEED_MODE_SDR12;
		break;

	case SDHC_TIMING_SDR50:
		mode = CDNS_SDHC_UHS_SPEED_MODE_SDR50;
		break;

	case SDHC_TIMING_HS200:
	case SDHC_TIMING_SDR104:
		mode = CDNS_SDHC_UHS_SPEED_MODE_SDR104;
		break;

	case SDHC_TIMING_DDR50:
	case SDHC_TIMING_DDR52:
		mode = CDNS_SDHC_UHS_SPEED_MODE_DDR50;
		break;

	case SDHC_TIMING_HS400:
		mode = CDNS_SDHC_UHS_SPEED_MODE_DDR200;
		break;

	default:
		return -EINVAL;
	}

	/* Select one of UHS mode */
	if (timing > SDHC_TIMING_HS) {
		cdns_sdhc_clear_bits(dev, CDNS_SDHC_AUTO_CMD_HOST_CTRL2,
				     CDNS_SDHC_HOST_CTRL2_UMS_MASK);
		cdns_sdhc_set_bits(dev, CDNS_SDHC_AUTO_CMD_HOST_CTRL2,
				   mode & CDNS_SDHC_HOST_CTRL2_UMS_MASK);
	}

	// hrs06 hook /* sdhci_cdns_set_control_reg() ->  */

	// tmp = readl(plat->hrs_addr + SDHCI_CDNS_HRS06);
	// tmp &= ~SDHCI_CDNS_HRS06_MODE;
	// tmp |= FIELD_PREP(SDHCI_CDNS_HRS06_MODE, mode);
	// writel(tmp, plat->hrs_addr + SDHCI_CDNS_HRS06);

	// if (config->has_phy == true) {
	// 	ret = cdns_sdhc_config_dll_clock(dev, mode);
	// 	if (ret != 0) {
	// 		LOG_ERR("Failed to config dll clock");
	// 	}
	// }

	return 0;
}

/**
 * @brief
 * Set voltage, power, clock, timing, bus width on host controller
 */
static int cdns_sdhc_set_io(const struct device *dev, struct sdhc_io *ios)
{
	struct cdns_sdhc_data *sd_data = dev->data;
	struct sdhc_io *host_io = (struct sdhc_io *)&sd_data->host_io;
	int ret;

	/* Check given clock is valid */
	if ((ios->clock != 0) &&
	    ((ios->clock > sd_data->props.f_max) || (ios->clock < sd_data->props.f_min))) {
		LOG_ERR("Invalid clock value");
		return -EINVAL;
	}

	/* Set power on or off */
	if (ios->power_mode != host_io->power_mode) {
		cdns_sdhc_set_power(dev, ios->power_mode);
		host_io->power_mode = ios->power_mode;
	}

	/* Set voltage level */
	if (ios->signal_voltage != host_io->signal_voltage) {
		ret = cdns_sdhc_set_voltage(dev, ios->signal_voltage);
		if (ret != 0) {
			LOG_ERR("Failed to set voltage level");
			return ret;
		}
		host_io->signal_voltage = ios->signal_voltage;
	}

	/* Set speed mode */
	if (ios->timing != host_io->timing) {
		ret = cdns_sdhc_set_timing(dev, ios->timing);
		if (ret != 0) {
			LOG_ERR("Failed to set speed mode");
			return ret;
		}
		host_io->timing = ios->timing;
	}

	/* Set clock */
	if (ios->clock != host_io->clock) {
		ret = cdns_sdhc_set_clock(dev, ios->clock);
		if (ret != 0) {
			LOG_ERR("Failed to set clock");
			return ret;
		}
		host_io->clock = ios->clock;
	}

	/* Set bus width */
	if (ios->bus_width != host_io->bus_width) {
		ret = cdns_sdhc_set_buswidth(dev, ios->bus_width);
		if (ret != 0) {
			LOG_ERR("Failed to set bus width");
			return ret;
		}
		host_io->bus_width = ios->bus_width;
	}

	return 0;
}

/**
 * @brief
 * Perform reset and enable status registers
 */
static int cdns_sdhc_host_reset(const struct device *dev)
{
	const struct cdns_sdhc_config *config = dev->config;
	int ret;

	/* Perform software reset (SRFA); this also clears the clock config
	 * that shares this 32-bit register.
	 */
	cdns_sdhc_write(dev, CDNS_SDHC_CLOCK_CTRL, CDNS_SDHC_CLOCK_CTRL_SRFA);
	/* Wait max 100ms for software reset to complete */
	ret = cdns_sdhc_wait_reg_mask(dev, CDNS_SDHC_CLOCK_CTRL, 100, CDNS_SDHC_CLOCK_CTRL_SRFA, 0);
	if (ret != 0) {
		LOG_ERR("Device is busy");
		return -EBUSY;
	}

	/* Enable status reg and configure interrupt (normal + error together) */
	cdns_sdhc_write(dev, CDNS_SDHC_INT_ENABLE,
			CDNS_SDHC_NORMAL_INT_MASK | CDNS_SDHC_ERR_INT_MASK);

	if (config->irq_config_fn == NULL) {
		cdns_sdhc_write(dev, CDNS_SDHC_INT_SIGNAL_ENABLE, 0);
	} else {
		/*
		 * Enable command complete, transfer complete, read buffer ready and
		 * error status interrupt
		 */
		cdns_sdhc_write(dev, CDNS_SDHC_INT_SIGNAL_ENABLE, CDNS_SDHC_TXFR_INTR_EN_MASK);
	}

	/* Data line timeout interval (DTCV field, shares CDNS_SDHC_CLOCK_CTRL) */
	cdns_sdhc_write(dev, CDNS_SDHC_CLOCK_CTRL,
			(CDNS_SDHC_CLOCK_CTRL_DTCV << CDNS_SDHC_CLOCK_CTRL_DTCV_POS) &
				CDNS_SDHC_CLOCK_CTRL_DTCV_MASK);

	/* Select DMA mode (DMA Select field, shares CDNS_SDHC_HOST_CTRL) */
	cdns_sdhc_clear_bits(dev, CDNS_SDHC_HOST_CTRL, CDNS_SDHC_HOST_CTRL_DMASEL_MASK);
#if defined(CONFIG_CDNS_SDHC_ADMA2_SUPPORT)
	cdns_sdhc_set_bits(dev, CDNS_SDHC_HOST_CTRL, CDNS_SDHC_HOST_CTRL_DMASEL_ADMA2_64);
#endif
	/* DMASEL == 0b00 already selects SDMA, nothing further to set there */

	cdns_sdhc_write(dev, CDNS_SDHC_BLOCK_SIZE, CDNS_SDHC_BLOCK_SIZE_512);

	cdns_sdhc_clear_intr(dev);

	return 0;
}

/**
 * @brief
 * Check for card busy
 */
static int cdns_sdhc_card_busy(const struct device *dev)
{
	int ret;

	/* Wait max 2ms for card to send next command */
	ret = cdns_sdhc_wait_reg_mask(dev, CDNS_SDHC_PRESENT_STATE, 2,
				      CDNS_SDHC_PRESENT_STATE_DATSL1_MASK, 0);
	if (ret != 0) {
		return 0;
	}

	return 1;
}

/**
 * @brief
 * Perform early system init for SDHC
 */
static int cdns_sdhc_init(const struct device *dev)
{
	const struct cdns_sdhc_config *config = dev->config;
	struct cdns_sdhc_data *sd_data = dev->data;

	DEVICE_MMIO_NAMED_MAP(dev, srs, K_MEM_CACHE_NONE);
	
	if (device_is_ready(config->clock_dev) == 0) {
		LOG_ERR("Clock control device not ready");
		return -ENODEV;
	}

	if (config->irq_config_fn != NULL) {
		k_event_init(&sd_data->irq_event);
		config->irq_config_fn(dev);
	}

	return cdns_sdhc_host_reset(dev);
}

static DEVICE_API(sdhc, cdns_sdhc_api) = {
	.reset = cdns_sdhc_host_reset,
	.request = cdns_sdhc_request,
	.set_io = cdns_sdhc_set_io,
	.get_card_present = cdns_sdhc_card_detect,
	.card_busy = cdns_sdhc_card_busy,
	.get_host_props = cdns_sdhc_host_props,
};

#define CDNS_SDHC_INTR_CONFIG(n)                                                                   \
	static void cdns_sdhc_irq_handler##n(const struct device *dev)                             \
	{                                                                                          \
		struct cdns_sdhc_data *sd_data = dev->data;                                        \
		uint32_t status = cdns_sdhc_read(dev, CDNS_SDHC_INT_STATUS);                       \
		if ((status & CDNS_SDHC_INT_STATUS_CC) != 0U) {                                    \
			cdns_sdhc_write(dev, CDNS_SDHC_INT_STATUS, CDNS_SDHC_INT_STATUS_CC);       \
			k_event_post(&sd_data->irq_event, CDNS_SDHC_INT_STATUS_CC);                \
		}                                                                                  \
		if ((status & CDNS_SDHC_INT_STATUS_BRR) != 0U) {                                   \
			cdns_sdhc_write(dev, CDNS_SDHC_INT_STATUS, CDNS_SDHC_INT_STATUS_BRR);      \
			k_event_post(&sd_data->irq_event, CDNS_SDHC_INT_STATUS_BRR);               \
		}                                                                                  \
		if ((status & CDNS_SDHC_INT_STATUS_TC) != 0U) {                                    \
			cdns_sdhc_write(dev, CDNS_SDHC_INT_STATUS, CDNS_SDHC_INT_STATUS_TC);       \
			k_event_post(&sd_data->irq_event, CDNS_SDHC_INT_STATUS_TC);                \
		}                                                                                  \
		if ((status & CDNS_SDHC_ERR_INT_MASK) != 0U) {                                     \
			cdns_sdhc_write(dev, CDNS_SDHC_INT_STATUS, CDNS_SDHC_ERR_INT_MASK);        \
			k_event_post(&sd_data->irq_event, CDNS_SDHC_ERR_INT_MASK);                 \
		}                                                                                  \
	}                                                                                          \
	static void cdns_sdhc_config_intr##n(const struct device *dev)                             \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), cdns_sdhc_irq_handler##n,   \
			    DEVICE_DT_INST_GET(n), DT_INST_IRQ(n, flags));                         \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}
#define CDNS_SDHC_INTR_FUNC_REG(n) .irq_config_fn = cdns_sdhc_config_intr##n,

#define CDNS_SDHC_INTR_CONFIG_NULL
#define CDNS_SDHC_INTR_FUNC_REG_NULL .irq_config_fn = NULL,

#define CDNS_SDHC_INTR_CONFIG_API(n)                                                               \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(n, interrupts),                                          \
		(CDNS_SDHC_INTR_CONFIG(n)), (CDNS_SDHC_INTR_CONFIG_NULL))

#define CDNS_SDHC_INTR_FUNC_REG_API(n)                                                             \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(n, interrupts),                                          \
		(CDNS_SDHC_INTR_FUNC_REG(n)), (CDNS_SDHC_INTR_FUNC_REG_NULL))

#define CDNS_SDHC_INIT(n)                                                                          \
	CDNS_SDHC_INTR_CONFIG_API(n)                                                               \
	const static struct cdns_sdhc_config cdns_sdhc_config_##n = {                              \
		DEVICE_MMIO_NAMED_ROM_INIT_BY_NAME(srs, DT_DRV_INST(n)),                           \
		CDNS_SDHC_INTR_FUNC_REG_API(n).broken_cd = DT_INST_PROP_OR(n, broken_cd, 0),       \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.freq_max = DT_INST_PROP(n, max_bus_freq),                                         \
		.freq_min = DT_INST_PROP(n, min_bus_freq),                                         \
		.powerdelay = DT_INST_PROP_OR(n, power_delay_ms, 0),                               \
		.hs200_mode = DT_INST_PROP_OR(n, mmc_hs200_1_8v, 0),                               \
		.hs400_mode = DT_INST_PROP_OR(n, mmc_hs400_1_8v, 0),                               \
	};                                                                                         \
	static struct cdns_sdhc_data cdns_sdhc_data##n;                                            \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, cdns_sdhc_init, NULL, &cdns_sdhc_data##n, &cdns_sdhc_config_##n,  \
			      POST_KERNEL, CONFIG_SDHC_INIT_PRIORITY, &cdns_sdhc_api);

DT_INST_FOREACH_STATUS_OKAY(CDNS_SDHC_INIT)
