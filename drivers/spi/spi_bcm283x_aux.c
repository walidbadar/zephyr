/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT brcm_bcm283x_aux_spi

#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/pinctrl.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(bcm283x_aux_spi, CONFIG_SPI_LOG_LEVEL);

#include "spi_rtio.h"
#include "spi_context.h"

/* SPI register offsets */
#define BCM283X_AUX_SPI_CNTL0  0x00
#define BCM283X_AUX_SPI_CNTL1  0x04
#define BCM283X_AUX_SPI_STAT   0x08
#define BCM283X_AUX_SPI_PEEK   0x0C
#define BCM283X_AUX_SPI_IO     0x20
#define BCM283X_AUX_SPI_TXHOLD 0x30

/* Bitfields in CNTL0 */
#define BCM283X_AUX_SPI_CNTL0_SPEED(x)    (((x) << 20) & GENMASK(31, 20))
#define BCM283X_AUX_SPI_CNTL0_CS(x)       (((x) << 17) & GENMASK(19, 17))
#define BCM283X_AUX_SPI_CNTL0_POSTINPUT   BIT(16)
#define BCM283X_AUX_SPI_CNTL0_VAR_CS      BIT(15)
#define BCM283X_AUX_SPI_CNTL0_VAR_WIDTH   BIT(14)
#define BCM283X_AUX_SPI_CNTL0_DOUTHOLD(x) (((x) << 12) & GENMASK(13, 12))
#define BCM283X_AUX_SPI_CNTL0_ENABLE      BIT(11)
#define BCM283X_AUX_SPI_CNTL0_IN_RISING   BIT(10)
#define BCM283X_AUX_SPI_CNTL0_CLEARFIFO   BIT(9)
#define BCM283X_AUX_SPI_CNTL0_OUT_RISING  BIT(8)
#define BCM283X_AUX_SPI_CNTL0_CPOL        BIT(7)
#define BCM283X_AUX_SPI_CNTL0_MSBF_OUT    BIT(6)
#define BCM283X_AUX_SPI_CNTL0_SHIFTLEN(x) ((x) & GENMASK(5, 0))

/* Bitfields in CNTL1 */
#define BCM283X_AUX_SPI_CNTL1_CSHIGH(x) (((x) << 8) & GENMASK(10, 8))
#define BCM283X_AUX_SPI_CNTL1_TXEMPTY   BIT(7)
#define BCM283X_AUX_SPI_CNTL1_IDLE      BIT(6)
#define BCM283X_AUX_SPI_CNTL1_MSBF_IN   BIT(1)
#define BCM283X_AUX_SPI_CNTL1_KEEP_IN   BIT(0)

/* Bitfields in STAT */
#define BCM283X_AUX_SPI_STAT_TX_LVL(x)   (((x) << 24) & GENMASK(31, 24))
#define BCM283X_AUX_SPI_STAT_RX_LVL(x)   (((x) << 16) & GENMASK(23, 16))
#define BCM283X_AUX_SPI_STAT_TX_FULL     BIT(10)
#define BCM283X_AUX_SPI_STAT_TX_EMPTY    BIT(9)
#define BCM283X_AUX_SPI_STAT_RX_FULL     BIT(8)
#define BCM283X_AUX_SPI_STAT_RX_EMPTY    BIT(7)
#define BCM283X_AUX_SPI_STAT_BUSY        BIT(6)
#define BCM283X_AUX_SPI_STAT_BITCOUNT(x) ((x) & GENMASK(5, 0))

/* Clock Phase bits */
#define BCM283X_AUX_SPI_CPOL (BCM283X_AUX_SPI_CNTL0_CPOL | BCM283X_AUX_SPI_CNTL0_OUT_RISING)
#define BCM283X_AUX_SPI_CPOH (BCM283X_AUX_SPI_CNTL0_IN_RISING)

#define BCM283X_AUX_SPI_BYTE_LS(val, shift) (((val) >> ((shift) * 8)) & 0xFF)
#define BCM283X_AUX_SPI_BYTE_RS(val, shift) ((val & 0xFF) << ((shift) * 8))

#define BCM283X_AUX_SPI_FREQ(pclk, spi_clk) (DIV_ROUND_UP(pclk, (2 * spi_clk)) - 1)
#define BCM283X_AUX_SPI_FREQ_MAX            (MHZ(4))

#define BCM283X_AUX_SPI_CNTL0_DEFAULT (BCM283X_AUX_SPI_CNTL0_ENABLE | BCM283X_AUX_SPI_CNTL0_VAR_WIDTH | BCM283X_AUX_SPI_CNTL0_MSBF_OUT)
#define BCM283X_AUX_SPI_CNTL1_DEFAULT (BCM283X_AUX_SPI_CNTL1_MSBF_IN)

struct bcm283x_aux_spi_config {
	DEVICE_MMIO_ROM;
	const struct pinctrl_dev_config *pincfg;
};

struct bcm283x_aux_spi_data {
	DEVICE_MMIO_RAM;
	struct spi_context ctx;
	uint32_t pclk;
	uint32_t tx_count;
	uint32_t rx_count;
	uint32_t pending;
	struct k_spinlock lock;
};

static inline uint32_t bcm283x_aux_spi_read(const struct device *dev, uint32_t reg)
{
	return sys_read32(DEVICE_MMIO_GET(dev) + reg);
}

static inline void bcm283x_aux_spi_write(const struct device *dev, uint32_t reg, uint32_t data)
{
	sys_write32(data, DEVICE_MMIO_GET(dev) + reg);
}

static inline void bcm283x_aux_spi_reset(const struct device *dev)
{
	bcm283x_aux_spi_write(dev, BCM283X_AUX_SPI_CNTL1, 0);
	bcm283x_aux_spi_write(dev, BCM283X_AUX_SPI_CNTL0, BCM283X_AUX_SPI_CNTL0_CLEARFIFO);
}

static void bcm283x_aux_spi_drain_fifo(const struct device *dev)
{
	struct bcm283x_aux_spi_data *data = dev->data;
	struct spi_context *ctx = data->ctx;
	uint32_t value;
	uint8_t count;
	uint8_t i, j = 0;

	count = min(data->rx_count, 3);
	value = bcm283x_aux_spi_read(dev, BCM283X_AUX_SPI_IO);
	for (i = count - 1; i >= 0; i--, j++) {
		ctx->rx_buf[j] = BCM283X_AUX_SPI_BYTE_LS(value, i);
	}

	data->rx_count -= count;
	data->pending -= count;
}

static void bcm283x_aux_spi_fill_fifo(const struct device *dev)
{
	struct bcm283x_aux_spi_data *data = dev->data;
	struct spi_context *ctx = data->ctx;
	uint32_t value = 0;
	uint8_t count;
	uint8_t i, j = 0;

	count = min(data->rx_count, 3);

	for (i = count - 1; i >= 0; i--, j++) {
		value |= BCM283X_AUX_SPI_BYTE_RS(ctx->tx_buf[j], i);
	}

	/* Set the variable bit-length */
	value |= (count * 8) << 24;

	/* Decrement length */
	data->tx_count -= count;
	data->pending += count;

	if (data->tx_count) {
		bcm283x_aux_spi_write(dev, BCM283X_AUX_SPI_TXHOLD, value);
	} else {
		bcm283x_aux_spi_write(dev, BCM283X_AUX_SPI_IO, value);
	}
}

static int bcm283x_aux_spi_configure(const struct device *dev, const struct spi_config *spicfg)
{
	const struct bcm283x_aux_spi_config *config = dev->config;
	struct bcm283x_aux_spi_data *data = dev->data;
	const uint16_t op = spicfg->operation;
	uint32_t cr0 = 0;
	uint32_t cr1 = 0;
	uint32_t freq;
	int ret;

	if (spi_context_configured(&data->ctx, spicfg)) {
		return 0;
	}

	if (spicfg->frequency > data->pclk / 2) {
		LOG_ERR("SPI Frequency is not %d supported.", spicfg->frequency);
		return -ENOTSUP;
	}

	if (op & SPI_TRANSFER_LSB) {
		LOG_ERR("LSB-first not supported");
		return -ENOTSUP;
	}

	/* Half-duplex mode has not been implemented */
	if (op & SPI_HALF_DUPLEX) {
		LOG_ERR("Half-duplex not supported");
		return -ENOTSUP;
	}

	/* Peripheral mode has not been implemented */
	if (SPI_OP_MODE_GET(op) != SPI_OP_MODE_MASTER) {
		LOG_ERR("Peripheral mode is not supported");
		return -ENOTSUP;
	}

	/* Word sizes other than 8 bits has not been implemented */
	if (SPI_WORD_SIZE_GET(op) != 8) {
		LOG_ERR("Word sizes other than 8 bits are not supported");
		return -ENOTSUP;
	}

	freq = BCM283X_AUX_SPI_FREQ(data->pclk, spicfg->frequency);
	if (freq > BCM283X_AUX_SPI_FREQ_MAX) {
		LOG_WRN("SPI Controller will use max frequency: %d", BCM283X_AUX_SPI_FREQ_MAX);
		freq = BCM283X_AUX_SPI_FREQ_MAX;
	}

	cr0 |= BCM283X_AUX_SPI_CNTL0_SPEED(freq);
	cr0 |= (SPI_WORD_SIZE_GET(op) - 1);
	cr0 |= (op & SPI_MODE_CPOL) ? BCM283X_AUX_SPI_CPOL : BCM283X_AUX_SPI_CPOH;
	cr0 |= BCM283X_AUX_SPI_CNTL0_DEFAULT;
	cr1 |= BCM283X_AUX_SPI_CNTL1_DEFAULT;
	
	bcm283x_aux_spi_write(dev, BCM283X_AUX_SPI_CNTL0, cr0);
	bcm283x_aux_spi_write(dev, BCM283X_AUX_SPI_CNTL1, cr1);

	data->ctx.config = spicfg;

	return 0;
}

static inline bool bcm283x_aux_spi_transfer_ongoing(struct bcm283x_aux_spi_data *data)
{
	return spi_context_tx_on(&data->ctx) || spi_context_rx_on(&data->ctx);
}

static void bcm283x_aux_spi_xfer(const struct device *dev)
{
	const struct bcm283x_aux_spi_config *config = dev->config;
	struct bcm283x_aux_spi_data *data = dev->data;
	const size_t chunk_len = spi_context_max_continuous_chunk(&data->ctx);
	const void *txbuf = data->ctx.tx_buf;
	void *rxbuf = data->ctx.rx_buf;
	uint32_t txrx;
	size_t fifo_cnt = 0;

	data->tx_count = 0;
	data->rx_count = 0;

	/* Ensure writable */
	while (!SSP_TX_FIFO_EMPTY(config->reg)) {
		;
	}
	/* Drain RX FIFO */
	while (SSP_RX_FIFO_NOT_EMPTY(config->reg)) {
		SSP_READ_REG(SSP_DR(config->reg));
	}

	while (data->rx_count < chunk_len || data->tx_count < chunk_len) {
		/* Fill up fifo with available TX data */
		while (SSP_TX_FIFO_NOT_FULL(config->reg) && data->tx_count < chunk_len &&
		       fifo_cnt < SSP_FIFO_DEPTH) {
			/* Send 0 in the case of read only operation */
			txrx = 0;

			if (txbuf) {
				txrx = ((uint8_t *)txbuf)[data->tx_count];
			}
			SSP_WRITE_REG(SSP_DR(config->reg), txrx);
			data->tx_count++;
			fifo_cnt++;
		}
		while (data->rx_count < chunk_len && fifo_cnt > 0) {
			if (!SSP_RX_FIFO_NOT_EMPTY(config->reg)) {
				continue;
			}

			txrx = SSP_READ_REG(SSP_DR(config->reg));

			/* Discard received data if rx buffer not assigned */
			if (rxbuf) {
				((uint8_t *)rxbuf)[data->rx_count] = (uint8_t)txrx;
			}
			data->rx_count++;
			fifo_cnt--;
		}
	}
}

static int bcm283x_aux_spi_transceive_impl(const struct device *dev,
					   const struct spi_config *config,
					   const struct spi_buf_set *tx_bufs,
					   const struct spi_buf_set *rx_bufs)
{
	const struct bcm283x_aux_spi_config *config = dev->config;
	struct bcm283x_aux_spi_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;
	int ret;

	spi_context_lock(&data->ctx, false, NULL, NULL, config);

	ret = bcm283x_aux_spi_configure(dev, config);
	if (ret < 0) {
		goto error;
	}

	spi_context_buffers_setup(ctx, tx_bufs, rx_bufs, 1);

	spi_context_cs_control(ctx, true);

	do {
		bcm283x_aux_spi_xfer(dev);
		spi_context_update_tx(ctx, 1, data->tx_count);
		spi_context_update_rx(ctx, 1, data->rx_count);
	} while (bcm283x_aux_spi_transfer_ongoing(data));

#if defined(CONFIG_SPI_ASYNC)
	spi_context_complete(&data->ctx, dev, ret);
#endif

	spi_context_cs_control(ctx, false);

error:
	spi_context_release(&data->ctx, ret);

	return ret;
}

static int bcm283x_aux_spi_transceive(const struct device *dev, const struct spi_config *config,
				      const struct spi_buf_set *tx_bufs,
				      const struct spi_buf_set *rx_bufs)
{
	return bcm283x_aux_spi_transceive_impl(dev, config, tx_bufs, rx_bufs, NULL, NULL);
}

#if defined(CONFIG_SPI_ASYNC)
static int bcm283x_aux_spi_transceive_async(const struct device *dev,
					    const struct spi_config *config,
					    const struct spi_buf_set *tx_bufs,
					    const struct spi_buf_set *rx_bufs, spi_callback_t cb,
					    void *userdata)
{
	return bcm283x_aux_spi_transceive_impl(dev, config, tx_bufs, rx_bufs, cb, userdata);
}
#endif

static int bcm283x_aux_spi_release(const struct device *dev, const struct spi_config *config)
{
	struct bcm283x_aux_spi_data *data = dev->data;

	spi_context_unlock_unconditionally(&data->ctx);
	return 0;
}

static DEVICE_API(spi, bcm283x_aux_spi_api) = {
	.transceive = bcm283x_aux_spi_transceive,
#if defined(CONFIG_SPI_ASYNC)
	.transceive_async = bcm283x_aux_spi_transceive_async,
#endif
#ifdef CONFIG_SPI_RTIO
	.iodev_submit = spi_rtio_iodev_default_submit,
#endif
	.release = bcm283x_aux_spi_release
};

static int bcm283x_aux_spi_init(const struct device *dev)
{
	/* Initialize with lowest frequency */
	const struct spi_config spicfg = {
		.frequency = 0,
		.operation = SPI_WORD_SET(8),
		.slave = 0,
	};
	const struct bcm283x_aux_spi_config *config = dev->config;
	struct bcm283x_aux_spi_data *data = dev->data;
	int ret;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		LOG_ERR("Failed to apply pinctrl state");
		return ret;
	}

	ret = bcm283x_aux_spi_configure(dev, &spicfg);
	if (ret < 0) {
		LOG_ERR("Failed to configure spi");
		return ret;
	}

	ret = spi_context_cs_configure_all(&data->ctx);
	if (ret < 0) {
		LOG_ERR("Failed to configure spi context");
		return ret;
	}

	/* Make sure the context is unlocked */
	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

#define BCM283X_AUX_SPI_PINCTRL_DEFINE(n) PINCTRL_DT_INST_DEFINE(n)

#define BCM283X_AUX_SPI_DEV_DATA(n)                                                                \
	static struct bcm283x_aux_spi_data bcm283x_aux_spi_data_##n = {                            \
		.pclk = DT_INST_PROP_BY_PHANDLE(n, clocks, clock_frequency),                       \
		SPI_CONTEXT_INIT_LOCK(bcm283x_aux_spi_data_##n, ctx),                              \
		SPI_CONTEXT_INIT_SYNC(bcm283x_aux_spi_data_##n, ctx),                              \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(n), ctx),                              \
	}

#define BCM283X_AUX_SPI_DEV_CFG(n)                                                                 \
	static const struct bcm283x_aux_spi_config bcm283x_aux_spi_config_##n = {                  \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)),                                              \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                       \
	}

#define BCM283X_AUX_SPI_INIT(n)                                                                    \
	DEVICE_DT_INST_DEFINE(n, bcm283x_aux_spi_init, NULL, &bcm283x_aux_spi_data_##n,            \
			      &bcm283x_aux_spi_config_##n, POST_KERNEL, CONFIG_SPI_INIT_PRIORITY,  \
			      &bcm283x_aux_spi_api)

#define BCM283X_AUX_SPI_INSTANTIATE(n)                                                             \
	BCM283X_AUX_SPI_PINCTRL_DEFINE(n);                                                         \
	BCM283X_AUX_SPI_DEV_DATA(n);                                                               \
	BCM283X_AUX_SPI_DEV_CFG(n);                                                                \
	BCM283X_AUX_SPI_INIT(n);

DT_INST_FOREACH_STATUS_OKAY(BCM283X_AUX_SPI_INSTANTIATE)
