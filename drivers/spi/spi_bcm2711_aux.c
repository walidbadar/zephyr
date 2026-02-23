/*
  * Copyright (c) 2026 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT brcm_bcm2711_aux_spi

#include <zephyr/kernel.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/util.h>
#include <zephyr/spinlock.h>
#include <soc.h>
#if defined(CONFIG_PINCTRL)
#include <zephyr/drivers/pinctrl.h>
#endif
#if defined(CONFIG_SPI_BCM2711_AUX_INTERRUPT)
#include <zephyr/irq.h>
#endif

#include <zephyr/logging/log.h>
#define LOG_LEVEL CONFIG_SPI_LOG_LEVEL
LOG_MODULE_REGISTER(bcm2711_aux_spi);

/* SPI register offsets */
#define BCM2711_AUX_SPI_CNTL0	0x00
#define BCM2711_AUX_SPI_CNTL1	0x04
#define BCM2711_AUX_SPI_STAT	0x08
#define BCM2711_AUX_SPI_PEEK	0x0C
#define BCM2711_AUX_SPI_IO	0x20
#define BCM2711_AUX_SPI_TXHOLD	0x30

/* Bitfields in CNTL0 */
#define BCM2711_AUX_SPI_CNTL0_SPEED			GENMASK(31, 20)
#define BCM2711_AUX_SPI_CNTL0_SPEED_MAX 	GENMASK(11, 0)
#define BCM2711_AUX_SPI_CNTL0_SPEED_SHIFT	20
#define BCM2711_AUX_SPI_CNTL0_CS			GENMASK(19, 17)
#define BCM2711_AUX_SPI_CNTL0_POSTINPUT		BIT(16)
#define BCM2711_AUX_SPI_CNTL0_VAR_CS		BIT(15)
#define BCM2711_AUX_SPI_CNTL0_VAR_WIDTH		BIT(14)
#define BCM2711_AUX_SPI_CNTL0_DOUTHOLD		GENMASK(13, 12)
#define BCM2711_AUX_SPI_CNTL0_ENABLE		BIT(11)
#define BCM2711_AUX_SPI_CNTL0_IN_RISING		BIT(10)
#define BCM2711_AUX_SPI_CNTL0_CLEARFIFO		BIT(9)
#define BCM2711_AUX_SPI_CNTL0_OUT_RISING	BIT(8)
#define BCM2711_AUX_SPI_CNTL0_CPOL			BIT(7)
#define BCM2711_AUX_SPI_CNTL0_MSBF_OUT		BIT(6)
#define BCM2711_AUX_SPI_CNTL0_SHIFTLEN		GENMASK(5, 0)

/* Bitfields in CNTL1 */
#define BCM2711_AUX_SPI_CNTL1_CSHIGH	GENMASK(10, 8)
#define BCM2711_AUX_SPI_CNTL1_TXEMPTY	BIT(7)
#define BCM2711_AUX_SPI_CNTL1_IDLE		BIT(6)
#define BCM2711_AUX_SPI_CNTL1_MSBF_IN	BIT(1)
#define BCM2711_AUX_SPI_CNTL1_KEEP_IN	BIT(0)

/* Bitfields in STAT */
#define BCM2711_AUX_SPI_STAT_TX_LVL		GENMASK(31, 24)
#define BCM2711_AUX_SPI_STAT_RX_LVL		GENMASK(23, 16)
#define BCM2711_AUX_SPI_STAT_TX_FULL	BIT(10)
#define BCM2711_AUX_SPI_STAT_TX_EMPTY	BIT(9)
#define BCM2711_AUX_SPI_STAT_RX_FULL	BIT(8)
#define BCM2711_AUX_SPI_STAT_RX_EMPTY	BIT(7)
#define BCM2711_AUX_SPI_STAT_BUSY		BIT(6)
#define BCM2711_AUX_SPI_STAT_BITCOUNT	GENMASK(5, 0)

#define BCM2711_AUX_SPI_BYTE_LS(val, n)  (((val) >> ((n) * 8)) & 0xFF)
#define BCM2711_AUX_SPI_BYTE_RS(val, n)  ((val) << ((n) * 8))

#define BCM2711_AUX_SPI_FREQ(pclk, speed_field) ((pclk) / (2 * (speed_field + 1)))

struct bcm2711_aux_spi_cfg {
	DEVICE_MMIO_ROM;
	const struct device *clock;
#if defined(CONFIG_PINCTRL)
	const struct pinctrl_dev_config *pincfg;
#endif
#if defined(CONFIG_SPI_BCM2711_AUX_INTERRUPT)
	void (*irq_config)(const struct device *port);
#endif
};

struct bcm2711_aux_spi_data {
	DEVICE_MMIO_RAM;
	struct spi_context ctx;
	uint32_t tx_count;
	uint32_t rx_count;
	struct k_spinlock lock;
};

static inline uint32_t bcm2711_aux_spi_read(const struct device *dev, uint32_t reg)
{
	return sys_read32(DEVICE_MMIO_GET(dev) + reg);
}

static inline void bcm2711_aux_spi_write(const struct device *dev, uint32_t reg,
				 uint32_t data)
{
	sys_write32(data, DEVICE_MMIO_GET(dev) + reg);
}

static void bcm2711_aux_spi_reset(const struct device *dev)
{
	bcm2711_aux_spi_write(dev, BCM2711_AUX_SPI_CNTL1, 0);
	bcm2711_aux_spi_write(dev, BCM2711_AUX_SPI_CNTL0,
		      BCM2711_AUX_SPI_CNTL0_CLEARFIFO);
}

static inline void bcm2711_aux_spi_drain_fifo(const struct device *dev)
{
	struct bcm2711_aux_spi_data *data = dev->data;
	struct spi_context *ctx = data->ctx;
	uint32_t value;
	int i, j = 0;

	uint8_t count = min(data->rx_count, 3);

	value = bcm2711_aux_spi_read(dev, BCM2711_AUX_SPI_IO);
	for (i = count - 1; i >= 0; i--, j++) {
		ctx->rx_buf[j] = BCM2711_AUX_SPI_BYTE_LS(value, i);
	}

	data->rx_count -= count;
	// dev->pending -= count; /* FIX ME */
}

static inline void bcm2711_aux_spi_fill_fifo(const struct device *dev)
{
	struct bcm2711_aux_spi_data *data = dev->data;
	struct spi_context *ctx = data->ctx;
	uint32_t value = 0;
	int i, j = 0;

	uint8_t count = min(data->rx_count, 3);

	for (i = count - 1; i >= 0; i--, j++) {
		value |= BCM2711_AUX_SPI_BYTE_RS(ctx->tx_buf[j], i);
	}

	/* and set the variable bit-length */
	value |= (count * 8) << 24;

	/* and decrement length */
	data->tx_count -= count;
	// dev->pending += count; /* FIX ME */

	/* write to the correct TX-register */
	if (data->tx_count) {
		bcm2711_aux_spi_write(dev, BCM2711_AUX_SPI_TXHOLD, value);
	} else {
		bcm2711_aux_spi_write(dev, BCM2711_AUX_SPI_IO, value);
	}
}

static int bcm2711_aux_spi_configure(const struct device *dev,
			       const struct spi_config *spicfg)
{
	const struct bcm2711_aux_spi_cfg *cfg = dev->config;
	struct bcm2711_aux_spi_data *data = dev->data;
	const uint16_t op = spicfg->operation;
	uint32_t prescale;
	uint32_t postdiv;
	uint32_t pclk = 0;
	uint32_t cr0;
	uint32_t cr1;
	int ret;

	if (spi_context_configured(&data->ctx, spicfg)) {
		return 0;
	}

	ret = clock_control_get_rate(cfg->clk_dev, cfg->clk_id, &pclk);
	if (ret < 0 || pclk == 0) {
		return -EINVAL;
	}

	if (spicfg->frequency > MAX_FREQ_CONTROLLER_MODE(pclk)) {
		LOG_ERR("Frequency is up to %u in controller mode.",
			MAX_FREQ_CONTROLLER_MODE(pclk));
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

	/* configure registers */

	prescale = bcm2711_aux_spi_calc_prescale(pclk, spicfg->frequency);
	postdiv = bcm2711_aux_spi_calc_postdiv(pclk, spicfg->frequency, prescale);

	cr0 = 0;
	cr0 |= (postdiv << SSP_CR0_SCR_LSB);
	cr0 |= (SPI_WORD_SIZE_GET(op) - 1);
	cr0 |= (op & SPI_MODE_CPOL) ? SSP_CR0_MASK_SPO : 0;
	cr0 |= (op & SPI_MODE_CPHA) ? SSP_CR0_MASK_SPH : 0;

	cr1 = 0;
	cr1 |= SSP_CR1_MASK_SSE; /* Always enable SPI */
	cr1 |= (op & SPI_MODE_LOOP) ? SSP_CR1_MASK_LBM : 0;

	/* Disable the SSP before it is reconfigured */
	SSP_WRITE_REG(SSP_CR1(cfg->reg), 0);
	SSP_WRITE_REG(SSP_CPSR(cfg->reg), prescale);
	SSP_WRITE_REG(SSP_CR0(cfg->reg), cr0);
	SSP_WRITE_REG(SSP_CR1(cfg->reg), cr1);

#if defined(CONFIG_SPI_BCM2711_AUX_INTERRUPT)
	SSP_WRITE_REG(SSP_IMSC(cfg->reg),
				SSP_IMSC_MASK_RORIM | SSP_IMSC_MASK_RTIM | SSP_IMSC_MASK_RXIM);
#endif

	data->ctx.config = spicfg;

	return 0;
}

static inline bool bcm2711_aux_spi_transfer_ongoing(struct bcm2711_aux_spi_data *data)
{
	return spi_context_tx_on(&data->ctx) || spi_context_rx_on(&data->ctx);
}

#if defined(CONFIG_SPI_BCM2711_AUX_INTERRUPT)

static void bcm2711_aux_spi_async_xfer(const struct device *dev)
{
	const struct bcm2711_aux_spi_cfg *cfg = dev->config;
	struct bcm2711_aux_spi_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;
	/* Process by per chunk */
	size_t chunk_len = spi_context_max_continuous_chunk(ctx);
	uint32_t txrx;

	/* Read RX FIFO */
	while (SSP_RX_FIFO_NOT_EMPTY(cfg->reg) && (data->rx_count < chunk_len)) {
		txrx = SSP_READ_REG(SSP_DR(cfg->reg));

		/* Discard received data if rx buffer not assigned */
		if (ctx->rx_buf) {
			*(((uint8_t *)ctx->rx_buf) + data->rx_count) = (uint8_t)txrx;
		}
		data->rx_count++;
	}

	/* Check transfer finished.
	 * The transmission of this chunk is complete if both the tx_count
	 * and the rx_count reach greater than or equal to the chunk_len.
	 * chunk_len is zero here means the transfer is already complete.
	 */
	if (MIN(data->tx_count, data->rx_count) >= chunk_len && chunk_len > 0) {
		spi_context_update_tx(ctx, 1, chunk_len);
		spi_context_update_rx(ctx, 1, chunk_len);
		if (bcm2711_aux_spi_transfer_ongoing(data)) {
			/* Next chunk is available, reset the count and continue processing */
			data->tx_count = 0;
			data->rx_count = 0;
			chunk_len = spi_context_max_continuous_chunk(ctx);
		} else {
			/* All data is processed, complete the process */
			spi_context_complete(ctx, dev, 0);
			return;
		}
	}

	/* Fill up TX FIFO */
	for (uint32_t i = 0; i < SSP_FIFO_DEPTH; i++) {
		if ((data->tx_count < chunk_len) && SSP_TX_FIFO_NOT_FULL(cfg->reg)) {
			/* Send 0 in the case of read only operation */
			txrx = 0;

			if (ctx->tx_buf) {
				txrx = *(((uint8_t *)ctx->tx_buf) + data->tx_count);
			}
			SSP_WRITE_REG(SSP_DR(cfg->reg), txrx);
			data->tx_count++;
		} else {
			break;
		}
	}
}

static void bcm2711_aux_spi_start_async_xfer(const struct device *dev)
{
	const struct bcm2711_aux_spi_cfg *cfg = dev->config;
	struct bcm2711_aux_spi_data *data = dev->data;

	/* Ensure writable */
	while (!SSP_TX_FIFO_EMPTY(cfg->reg)) {
		;
	}
	/* Drain RX FIFO */
	while (SSP_RX_FIFO_NOT_EMPTY(cfg->reg)) {
		SSP_READ_REG(SSP_DR(cfg->reg));
	}

	data->tx_count = 0;
	data->rx_count = 0;

	SSP_WRITE_REG(SSP_ICR(cfg->reg), SSP_ICR_MASK_RORIC | SSP_ICR_MASK_RTIC);

	bcm2711_aux_spi_async_xfer(dev);
}

static void bcm2711_aux_spi_isr(const struct device *dev)
{
	const struct bcm2711_aux_spi_cfg *cfg = dev->config;
	struct bcm2711_aux_spi_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;
	uint32_t mis = SSP_READ_REG(SSP_MIS(cfg->reg));

	if (mis & SSP_MIS_MASK_RORMIS) {
		SSP_WRITE_REG(SSP_IMSC(cfg->reg), 0);
		spi_context_complete(ctx, dev, -EIO);
	} else {
		bcm2711_aux_spi_async_xfer(dev);
	}

	SSP_WRITE_REG(SSP_ICR(cfg->reg), SSP_ICR_MASK_RORIC | SSP_ICR_MASK_RTIC);
}

#else

static void bcm2711_aux_spi_xfer(const struct device *dev)
{
	const struct bcm2711_aux_spi_cfg *cfg = dev->config;
	struct bcm2711_aux_spi_data *data = dev->data;
	const size_t chunk_len = spi_context_max_continuous_chunk(&data->ctx);
	const void *txbuf = data->ctx.tx_buf;
	void *rxbuf = data->ctx.rx_buf;
	uint32_t txrx;
	size_t fifo_cnt = 0;

	data->tx_count = 0;
	data->rx_count = 0;

	/* Ensure writable */
	while (!SSP_TX_FIFO_EMPTY(cfg->reg)) {
		;
	}
	/* Drain RX FIFO */
	while (SSP_RX_FIFO_NOT_EMPTY(cfg->reg)) {
		SSP_READ_REG(SSP_DR(cfg->reg));
	}

	while (data->rx_count < chunk_len || data->tx_count < chunk_len) {
		/* Fill up fifo with available TX data */
		while (SSP_TX_FIFO_NOT_FULL(cfg->reg) && data->tx_count < chunk_len &&
		       fifo_cnt < SSP_FIFO_DEPTH) {
			/* Send 0 in the case of read only operation */
			txrx = 0;

			if (txbuf) {
				txrx = ((uint8_t *)txbuf)[data->tx_count];
			}
			SSP_WRITE_REG(SSP_DR(cfg->reg), txrx);
			data->tx_count++;
			fifo_cnt++;
		}
		while (data->rx_count < chunk_len && fifo_cnt > 0) {
			if (!SSP_RX_FIFO_NOT_EMPTY(cfg->reg)) {
				continue;
			}

			txrx = SSP_READ_REG(SSP_DR(cfg->reg));

			/* Discard received data if rx buffer not assigned */
			if (rxbuf) {
				((uint8_t *)rxbuf)[data->rx_count] = (uint8_t)txrx;
			}
			data->rx_count++;
			fifo_cnt--;
		}
	}
}

#endif

static int bcm2711_aux_spi_transceive_impl(const struct device *dev,
				     const struct spi_config *config,
				     const struct spi_buf_set *tx_bufs,
				     const struct spi_buf_set *rx_bufs,
				     spi_callback_t cb,
				     void *userdata)
{
	const struct bcm2711_aux_spi_cfg *cfg = dev->config;
	struct bcm2711_aux_spi_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;
	int ret;

	spi_context_lock(&data->ctx, (cb ? true : false), cb, userdata, config);

	ret = bcm2711_aux_spi_configure(dev, config);
	if (ret < 0) {
		goto error;
	}

	spi_context_buffers_setup(ctx, tx_bufs, rx_bufs, 1);

	spi_context_cs_control(ctx, true);

#if defined(CONFIG_SPI_BCM2711_AUX_INTERRUPT)
		bcm2711_aux_spi_start_async_xfer(dev);
		ret = spi_context_wait_for_completion(ctx);
#endif
	do {
		bcm2711_aux_spi_xfer(dev);
		spi_context_update_tx(ctx, 1, data->tx_count);
		spi_context_update_rx(ctx, 1, data->rx_count);
	} while (bcm2711_aux_spi_transfer_ongoing(data));

	spi_context_cs_control(ctx, false);

error:
	spi_context_release(&data->ctx, ret);

	return ret;
}

static int bcm2711_aux_spi_transceive(const struct device *dev,
				const struct spi_config *config,
				const struct spi_buf_set *tx_bufs,
				const struct spi_buf_set *rx_bufs)
{
	return bcm2711_aux_spi_transceive_impl(dev, config, tx_bufs, rx_bufs, NULL, NULL);
}

static int bcm2711_aux_spi_release(const struct device *dev,
			     const struct spi_config *config)
{
	struct bcm2711_aux_spi_data *data = dev->data;

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static DEVICE_API(spi, bcm2711_aux_spi_api) = {
	.transceive = bcm2711_aux_spi_transceive,
	.release = bcm2711_aux_spi_release
};

static int bcm2711_aux_spi_init(const struct device *dev)
{
	/* Initialize with lowest frequency */
	const struct bcm2711_aux_spi_cfg *cfg = dev->config;
	struct bcm2711_aux_spi_data *data = dev->data;
	int ret;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	const struct spi_config spicfg = {
		.frequency = 0,
		.operation = SPI_WORD_SET(8),
		.slave = 0,
	};

#if defined(CONFIG_PINCTRL)
	ret = pinctrl_apply_state(cfg->pincfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		LOG_ERR("Failed to apply pinctrl state");
		return ret;
	}
#endif

#if defined(CONFIG_SPI_BCM2711_AUX_INTERRUPT)
		cfg->irq_config(dev);
#endif

	ret = bcm2711_aux_spi_configure(dev, &spicfg);
	if (ret < 0) {
		LOG_ERR("Failed to configure spi");
		return ret;
	}

	ret = spi_context_cs_configure_all(&data->ctx);
	if (ret < 0) {
		LOG_ERR("Failed to spi_context configure");
		return ret;
	}

	/* Make sure the context is unlocked */
	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

#define SPI_BCM2711_AUX_INIT(idx)                                                                        \
	IF_ENABLED(CONFIG_PINCTRL, (PINCTRL_DT_INST_DEFINE(idx);))                                 \
	IF_ENABLED(CONFIG_SPI_BCM2711_AUX_INTERRUPT,                                                     \
		   (static void bcm2711_aux_spi_irq_config_##idx(const struct device *dev)               \
		    {                                                                              \
			   IRQ_CONNECT(DT_INST_IRQN(idx), DT_INST_IRQ(idx, priority),              \
				       bcm2711_aux_spi_isr, DEVICE_DT_INST_GET(idx), 0);                 \
			   irq_enable(DT_INST_IRQN(idx));                                          \
		    }))                                                                            \
	IF_ENABLED(CONFIG_CLOCK_CONTROL, (CLOCK_ID_DECL(idx)))                                     \
	static struct bcm2711_aux_spi_data bcm2711_aux_spi_data_##idx = {                                      \
		SPI_CONTEXT_INIT_LOCK(bcm2711_aux_spi_data_##idx, ctx),                                  \
		SPI_CONTEXT_INIT_SYNC(bcm2711_aux_spi_data_##idx, ctx),                                  \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(idx), ctx)};                           \
	static struct bcm2711_aux_spi_cfg bcm2711_aux_spi_cfg_##idx = {                                        \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(port)),                                           \
		IF_ENABLED(CONFIG_CLOCK_CONTROL, (IF_ENABLED(DT_INST_NODE_HAS_PROP(0, clocks),     \
			(.clk_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(idx)),                       \
			 .clk_id = bcm2711_clk_id##idx,))))                                          \
		IF_ENABLED(CONFIG_PINCTRL, (.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(idx),))       \
		IF_ENABLED(CONFIG_SPI_BCM2711_AUX_INTERRUPT,                                             \
					   (.irq_config = bcm2711_aux_spi_irq_config_##idx,))};          \
	SPI_DEVICE_DT_INST_DEFINE(idx, bcm2711_aux_spi_init, NULL, &bcm2711_aux_spi_data_##idx,                \
			      &bcm2711_aux_spi_cfg_##idx, POST_KERNEL, CONFIG_SPI_INIT_PRIORITY,         \
			      &bcm2711_aux_spi_api);

DT_INST_FOREACH_STATUS_OKAY(SPI_BCM2711_AUX_INIT)
