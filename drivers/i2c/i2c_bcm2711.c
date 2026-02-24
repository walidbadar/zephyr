/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT brcm_bcm2711_i2c

#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/logging/log.h>

#define LOG_LEVEL CONFIG_I2C_LOG_LEVEL
LOG_MODULE_REGISTER(bcm2711_i2c);

#include "i2c-priv.h"

/* Register definitions */
#define BCM2711_I2C_CR   0x00
#define BCM2711_I2C_SR   0x04
#define BCM2711_I2C_DLEN 0x08
#define BCM2711_I2C_ADDR 0x0C
#define BCM2711_I2C_FIFO 0x10
#define BCM2711_I2C_DIV  0x14
#define BCM2711_I2C_DEL  0x18
#define BCM2711_I2C_CLKT 0x1C

/* Control register bits */
#define BCM2711_I2C_CR_READ  BIT(0)
#define BCM2711_I2C_CR_CLEAR BIT(4)
#define BCM2711_I2C_CR_ST    BIT(7)
#define BCM2711_I2C_CR_INTD  BIT(8)
#define BCM2711_I2C_CR_INTT  BIT(9)
#define BCM2711_I2C_CR_INTR  BIT(10)
#define BCM2711_I2C_CR_I2CEN BIT(15)

/* Status register bits */
#define BCM2711_I2C_SR_TA   BIT(0)
#define BCM2711_I2C_SR_DONE BIT(1)
#define BCM2711_I2C_SR_TXW  BIT(2)
#define BCM2711_I2C_SR_RXR  BIT(3)
#define BCM2711_I2C_SR_TXD  BIT(4)
#define BCM2711_I2C_SR_RXD  BIT(5)
#define BCM2711_I2C_SR_TXE  BIT(6)
#define BCM2711_I2C_SR_RXF  BIT(7)
#define BCM2711_I2C_SR_ERR  BIT(8)
#define BCM2711_I2C_SR_CLKT BIT(9)

#define BCM2711_I2C_EDGE_DELAY(divider) ((((divider) / 4) << 16) | ((divider) / 4))
#define BCM2711_I2C_TIMEOUT             100

struct bcm2711_i2c_config {
	DEVICE_MMIO_ROM;
	uint32_t bitrate;
	uint32_t pclk;
	void (*irq_config_func)(const struct device *dev);
	const struct pinctrl_dev_config *pincfg;
};

struct bcm2711_i2c_data {
	DEVICE_MMIO_RAM;
	struct k_sem dev_sync;
	uint16_t i2c_addr;
	struct i2c_msg *msgs;
	uint8_t num_msgs;
	int msg_err;
};

static inline uint32_t bcm2711_i2c_read_reg(const struct device *dev, uint32_t reg)
{
	mem_addr_t base = DEVICE_MMIO_GET(dev);
	return sys_read32(base + reg);
}

static inline void bcm2711_i2c_write_reg(const struct device *dev, uint32_t reg, uint32_t value)
{
	mem_addr_t base = DEVICE_MMIO_GET(dev);
	sys_write32(value, base + reg);
}

static void bcm2711_i2c_reset(const struct device *dev)
{
	/* Clear FIFO and reset state machine */
	bcm2711_i2c_write_reg(dev, BCM2711_I2C_CR, BCM2711_I2C_CR_CLEAR);

	/* Clear status flags */
	bcm2711_i2c_write_reg(dev, BCM2711_I2C_SR,
			      BCM2711_I2C_SR_CLKT | BCM2711_I2C_SR_ERR | BCM2711_I2C_SR_DONE);
}

static void bcm2711_i2c_send(const struct device *dev)
{
	struct bcm2711_i2c_data *data = dev->data;
	struct i2c_msg *msg = data->msgs;
	uint32_t status;

	for (; msg->len > 0; msg->len--, msg->buf++) {
		status = bcm2711_i2c_read_reg(dev, BCM2711_I2C_SR);

		if (!(status & BCM2711_I2C_SR_TXD)) {
			break;
		}

		bcm2711_i2c_write_reg(dev, BCM2711_I2C_FIFO, *msg->buf);
	}
}

static void bcm2711_i2c_recv(const struct device *dev)
{
	struct bcm2711_i2c_data *data = dev->data;
	struct i2c_msg *msg = data->msgs;
	uint32_t status;

	for (; msg->len > 0; msg->len--, msg->buf++) {
		status = bcm2711_i2c_read_reg(dev, BCM2711_I2C_SR);

		if (!(status & BCM2711_I2C_SR_RXD)) {
			break;
		}

		*msg->buf = bcm2711_i2c_read_reg(dev, BCM2711_I2C_FIFO);
	}
}

static void bcm2711_i2c_transaction(const struct device *dev)
{
	struct bcm2711_i2c_data *data = dev->data;
	struct i2c_msg *msg = data->msgs;
	uint32_t ctrl;

	data->num_msgs--;

	/* Configure control register */
	ctrl = BCM2711_I2C_CR_ST | BCM2711_I2C_CR_I2CEN;

	if (i2c_is_read_op(msg)) {
		ctrl |= BCM2711_I2C_CR_READ | BCM2711_I2C_CR_INTR;
	} else {
		ctrl |= BCM2711_I2C_CR_INTT;
	}

	if (!data->num_msgs) {
		ctrl |= BCM2711_I2C_CR_INTD;
	}

	/* Set slave address and message length */
	bcm2711_i2c_write_reg(dev, BCM2711_I2C_ADDR, data->i2c_addr);
	bcm2711_i2c_write_reg(dev, BCM2711_I2C_DLEN, msg->len);

	/* Start transfer */
	bcm2711_i2c_write_reg(dev, BCM2711_I2C_CR, ctrl);
}

static int bcm2711_i2c_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t num_msgs,
				uint16_t i2c_addr)
{
	struct bcm2711_i2c_data *data = dev->data;
	int ret;

	if (!msgs || !num_msgs) {
		LOG_ERR("No messages to transfer");
		return -EINVAL;
	}

	data->i2c_addr = i2c_addr;
	data->msgs = msgs;
	data->num_msgs = num_msgs;
	data->msg_err = 0;

	bcm2711_i2c_transaction(dev);

	/* Wait for transfer completion with timeout */
	ret = k_sem_take(&data->dev_sync, K_MSEC(BCM2711_I2C_TIMEOUT));
	if (ret < 0) {
		LOG_ERR("I2C transfer timed out");
		bcm2711_i2c_reset(dev);
		return -ETIMEDOUT;
	}

	/* Check for transfer errors */
	ret = data->msg_err;
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static void bcm2711_i2c_isr(const struct device *dev)
{
	struct bcm2711_i2c_data *data = dev->data;
	struct i2c_msg *msg = data->msgs;
	uint32_t status;

	status = bcm2711_i2c_read_reg(dev, BCM2711_I2C_SR);

	if (status & (BCM2711_I2C_SR_CLKT | BCM2711_I2C_SR_ERR)) {
		data->msg_err = -EINVAL;
	}

	if (status & BCM2711_I2C_SR_DONE) {
		if (i2c_is_read_op(data->msgs)) {
			bcm2711_i2c_recv(dev);
			status = bcm2711_i2c_read_reg(dev, BCM2711_I2C_SR);
		}

		if (msg->len) {
			data->msg_err = -EINVAL;
		}

		bcm2711_i2c_reset(dev);
		k_sem_give(&data->dev_sync);
	}

	if (status & BCM2711_I2C_SR_TXW) {
		bcm2711_i2c_send(dev);
		if (data->num_msgs && !msg->len) {
			data->msgs++;
			bcm2711_i2c_transaction(dev);
		}
	}

	if (status & BCM2711_I2C_SR_RXR) {
		bcm2711_i2c_recv(dev);
	}
}

static int bcm2711_i2c_configure(const struct device *dev, uint32_t dev_config)
{
	const struct bcm2711_i2c_config *config = dev->config;
	uint32_t divider, edge_delay;

	/* Configure clock divider based on requested speed */
	switch (I2C_SPEED_GET(dev_config)) {
	case I2C_SPEED_STANDARD:
		LOG_DBG("Standard mode selected");
		divider = config->pclk / I2C_BITRATE_STANDARD;
		break;
	case I2C_SPEED_FAST:
		LOG_DBG("Fast mode selected");
		divider = config->pclk / I2C_BITRATE_FAST;
		break;
	default:
		LOG_ERR("Only Standard or Fast modes are supported");
		return -EINVAL;
	}

	/* Set clock divider */
	bcm2711_i2c_write_reg(dev, BCM2711_I2C_DIV, divider);

	/* Set edge delay */
	edge_delay = BCM2711_I2C_EDGE_DELAY(divider);
	bcm2711_i2c_write_reg(dev, BCM2711_I2C_DEL, edge_delay);

	return 0;
}

static int bcm2711_i2c_init(const struct device *dev)
{
	struct bcm2711_i2c_data *data = dev->data;
	const struct bcm2711_i2c_config *config = dev->config;
	uint32_t bitrate;
	int ret;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	k_sem_init(&data->dev_sync, 0, 1);

	/* Clear control registers and disable CLKT */
	bcm2711_i2c_write_reg(dev, BCM2711_I2C_CLKT, 0);
	bcm2711_i2c_write_reg(dev, BCM2711_I2C_CR, 0);

	bitrate = i2c_map_dt_bitrate(config->bitrate);

	ret = bcm2711_i2c_configure(dev, bitrate);
	if (ret < 0) {
		LOG_ERR("Failed to configure I2C controller");
		return ret;
	}

	config->irq_config_func(dev);

	LOG_DBG("BCM2711 I2C controller initialized successfully");
	return 0;
}

static DEVICE_API(i2c, bcm2711_i2c_driver_api) = {
	.configure = bcm2711_i2c_configure,
	.transfer = bcm2711_i2c_transfer,
};

#define BCM2711_I2C_PINCTRL_DEFINE(port) PINCTRL_DT_INST_DEFINE(port)

#define BCM2711_I2C_IRQ_CONF_FUNC(port)                                                            \
	static void bcm2711_i2c_irq_config_func_##port(const struct device *dev)                   \
	{                                                                                          \
		ARG_UNUSED(dev);                                                                   \
		IRQ_CONNECT(DT_INST_IRQN(port), DT_INST_IRQ(port, priority), bcm2711_i2c_isr,      \
			    DEVICE_DT_INST_GET(port), 0);                                          \
		irq_enable(DT_INST_IRQN(port));                                                    \
	}

#define BCM2711_I2C_DEV_DATA(port) static struct bcm2711_i2c_data bcm2711_i2c_data_##port

#define BCM2711_I2C_DEV_CFG(port)                                                                  \
	static const struct bcm2711_i2c_config bcm2711_i2c_config_##port = {                       \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(port)),                                           \
		.bitrate = DT_INST_PROP(port, clock_frequency),                                    \
		.pclk = DT_INST_PROP_BY_PHANDLE(port, clocks, clock_frequency),                    \
		.irq_config_func = bcm2711_i2c_irq_config_func_##port,                             \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(port),                                    \
	};

#define BCM2711_I2C_INIT(port)                                                                     \
	DEVICE_DT_INST_DEFINE(port, &bcm2711_i2c_init, NULL, &bcm2711_i2c_data_##port,             \
			      &bcm2711_i2c_config_##port, POST_KERNEL, CONFIG_I2C_INIT_PRIORITY,   \
			      &bcm2711_i2c_driver_api);

#define BCM2711_I2C_INSTANTIATE(inst)                                                              \
	BCM2711_I2C_PINCTRL_DEFINE(inst);                                                          \
	BCM2711_I2C_IRQ_CONF_FUNC(inst);                                                           \
	BCM2711_I2C_DEV_DATA(inst);                                                                \
	BCM2711_I2C_DEV_CFG(inst);                                                                 \
	BCM2711_I2C_INIT(inst);

DT_INST_FOREACH_STATUS_OKAY(BCM2711_I2C_INSTANTIATE)
