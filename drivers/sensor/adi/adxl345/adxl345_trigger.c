/*
 * Copyright (c) 2018 Analog Devices Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT adi_adxl345

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include "adxl345.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(ADXL345, CONFIG_SENSOR_LOG_LEVEL);

#if defined(CONFIG_ADXL345_TRIGGER_OWN_THREAD) || defined(CONFIG_ADXL345_TRIGGER_GLOBAL_THREAD)
/* FIXME: Fix this shit */
static void adxl345_thread_cb(const struct device *dev)
{
	const struct adxl345_dev_config *cfg = dev->config;
	struct adxl345_dev_data *drv_data = dev->data;
	uint8_t status1;
	int ret;

	/* Clear the status */
	if (adxl345_get_status(dev, &status1, NULL) < 0) {
		return;
	}

	if ((drv_data->drdy_handler != NULL) &&
		ADXL345_STATUS_DATA_RDY(status1)) {
		drv_data->drdy_handler(dev, drv_data->drdy_trigger);
	}

	if ((drv_data->act_handler != NULL) &&
		ADXL345_STATUS_ACTIVITY(status1)) {
		drv_data->act_handler(dev, drv_data->act_trigger);
	}

	ret = gpio_pin_interrupt_configure_dt(&cfg->interrupt2,
					      GPIO_INT_EDGE_TO_ACTIVE);
	__ASSERT(ret == 0, "Interrupt configuration failed");
}
#endif

static void adxl345_gpio_callback(const struct device *dev,
				  struct gpio_callback *cb, uint32_t pins)
{
	struct adxl345_dev_data *drv_data =
		CONTAINER_OF(cb, struct adxl345_dev_data, gpio_cb);
	const struct adxl345_dev_config *cfg = drv_data->dev->config;

	gpio_pin_interrupt_configure_dt(&cfg->interrupt, GPIO_INT_DISABLE);

	if (IS_ENABLED(CONFIG_ADXL345_STREAM)) {
		adxl345_stream_irq_handler(drv_data->dev);
	}

#if defined(CONFIG_ADXL345_TRIGGER_OWN_THREAD)
	k_sem_give(&drv_data->gpio_sem);
#elif defined(CONFIG_ADXL345_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&drv_data->work);
#endif
}

static void adxl345_gpio2_callback(const struct device *dev,
				  struct gpio_callback *cb, uint32_t pins)
{
	struct adxl345_dev_data *drv_data =
		CONTAINER_OF(cb, struct adxl345_dev_data, gpio2_cb);
	const struct adxl345_dev_config *cfg = drv_data->dev->config;

	gpio_pin_interrupt_configure_dt(&cfg->interrupt2, GPIO_INT_DISABLE);

#if defined(CONFIG_ADXL345_TRIGGER_OWN_THREAD)
	k_sem_give(&drv_data->gpio_sem);
#elif defined(CONFIG_ADXL345_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&drv_data->work);
#endif
}

#if defined(CONFIG_ADXL345_TRIGGER_OWN_THREAD)
static void adxl345_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct adxl345_dev_data *drv_data = p1;

	while (true) {
		k_sem_take(&drv_data->gpio_sem, K_FOREVER);
		adxl345_thread_cb(drv_data->dev);
	}
}

#elif defined(CONFIG_ADXL345_TRIGGER_GLOBAL_THREAD)
static void adxl345_work_cb(struct k_work *work)
{
	struct adxl345_dev_data *drv_data =
		CONTAINER_OF(work, struct adxl345_dev_data, work);

	adxl345_thread_cb(drv_data->dev);
}
#endif

static int adxl345_trigger_drdy_set(const struct device *dev,
			const struct sensor_trigger *trig,
			sensor_trigger_handler_t handler)
{
	const struct adxl345_dev_config *cfg = dev->config;
	struct adxl345_dev_data *drv_data = dev->data;
	int ret;

	if (cfg->interrupt.port == NULL) {
		LOG_ERR("interrupt is not supported");
		return -ENOTSUP;
	}

	gpio_pin_interrupt_configure_dt(&cfg->interrupt, GPIO_INT_DISABLE);

	drv_data->drdy_handler = handler;
	drv_data->drdy_trigger = trig;

	ret = adxl345_interrupt_config(dev, ADXL345_INT_MAP_DATA_RDY_MSK, ADXL345_SELECT_INT1);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int adxl345_trigger_anym_set(const struct device *dev,
			const struct sensor_trigger *trig,
			sensor_trigger_handler_t handler)
{
	const struct adxl345_dev_config *cfg = dev->config;
	struct adxl345_dev_data *drv_data = dev->data;
	int ret;

	if (cfg->interrupt2.port == NULL) {
		LOG_ERR("interrupt2 is not supported");
		return -ENOTSUP;
	}

	gpio_pin_interrupt_configure_dt(&cfg->interrupt2, GPIO_INT_DISABLE);

	drv_data->act_handler = handler;
	drv_data->act_trigger = trig;

	/* TODO: Remove this after testing */
	adxl345_reg_write_byte(dev, ADXL345_THRESH_ACT_REG, 36);

	ret = adxl345_reg_write_byte(dev, ADXL345_ACT_INACT_CTL_REG,
				ADXL345_ACT_AC_DC | ADXL345_ACT_X_EN |
				ADXL345_ACT_Y_EN | ADXL345_ACT_Z_EN);
	if (ret < 0) {
		return ret;
	}

	ret = adxl345_interrupt_config(dev, ADXL345_INT_MAP_ACT_MSK, ADXL345_SELECT_INT2);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

int adxl345_trigger_set(const struct device *dev,
			const struct sensor_trigger *trig,
			sensor_trigger_handler_t handler)
{
	if(trig == NULL || handler == NULL) {
		return -EINVAL;
	}

#ifdef CONFIG_ADXL345_STREAM
	(void)adxl345_configure_fifo(dev, ADXL345_FIFO_BYPASSED, ADXL345_INT2, 0);
#endif

	switch (trig->type) {
	case SENSOR_TRIG_DATA_READY:
		return adxl345_trigger_drdy_set(dev, trig, handler);
	case SENSOR_TRIG_MOTION:
		return adxl345_trigger_anym_set(dev, trig, handler);
	default:
		LOG_ERR("Unsupported sensor trigger");
		return -ENOTSUP;
	}

	return 0;
}

int adxl345_init_interrupt(const struct device *dev)
{
	const struct adxl345_dev_config *cfg = dev->config;
	struct adxl345_dev_data *drv_data = dev->data;
	int ret;

	drv_data->dev = dev;

#if defined(CONFIG_ADXL345_TRIGGER_OWN_THREAD)
	k_sem_init(&drv_data->gpio_sem, 0, K_SEM_MAX_LIMIT);

	k_thread_create(&drv_data->thread, drv_data->thread_stack,
			CONFIG_ADXL345_THREAD_STACK_SIZE,
			adxl345_thread, drv_data,
			NULL, NULL, K_PRIO_COOP(CONFIG_ADXL345_THREAD_PRIORITY),
			0, K_NO_WAIT);

	k_thread_name_set(&drv_data->thread, dev->name);
#elif defined(CONFIG_ADXL345_TRIGGER_GLOBAL_THREAD)
	drv_data->work.handler = adxl345_work_cb;
#endif

	/* Disable interrupts upon reset */
	ret = adxl345_reg_write_byte(dev, ADXL345_INT_ENABLE, 0);
	if (ret < 0) {
		return ret;
	}

	if (!gpio_is_ready_dt(&cfg->interrupt)) {
		/* API may return false even when ptr is NULL */
		if (cfg->interrupt.port != NULL) {
			LOG_ERR("device %s is not ready", cfg->interrupt.port->name);
			return -ENODEV;
		}

		LOG_DBG("interrupt not defined in DT");
		goto check_interrupt2;
	}

	ret = gpio_pin_configure_dt(&cfg->interrupt, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}

	gpio_init_callback(&drv_data->gpio_cb,
			   adxl345_gpio_callback,
			   BIT(cfg->interrupt.pin));

	ret = gpio_add_callback(cfg->interrupt.port, &drv_data->gpio_cb);
	if (ret < 0) {
		LOG_ERR("Failed to set gpio callback!");
		return ret;
	}

	LOG_INF("%s: int1 on %s.%02u", dev->name,
					cfg->interrupt.port->name,
					cfg->interrupt.pin);

check_interrupt2:
	if (!gpio_is_ready_dt(&cfg->interrupt2)) {
		/* API may return false even when ptr is NULL */
		if (cfg->interrupt2.port != NULL) {
			LOG_ERR("device %s is not ready", cfg->interrupt2.port->name);
			return -ENODEV;
		}

		LOG_DBG("interrupt2 not defined in DT");
		return 0;
	}

	/* any motion int2 gpio configuration */
	ret = gpio_pin_configure_dt(&cfg->interrupt2, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Could not configure %s.%02u",
			cfg->interrupt2.port->name, cfg->interrupt2.pin);
		return ret;
	}

	gpio_init_callback(&drv_data->gpio2_cb,
			   adxl345_gpio2_callback,
			   BIT(cfg->interrupt2.pin));

	ret = gpio_add_callback(cfg->interrupt2.port, &drv_data->gpio2_cb);
	if (ret < 0) {
		LOG_ERR("Could not add gpio int2 callback (%d)", ret);
		return ret;
	}

	LOG_INF("%s: int2 on %s.%02u", dev->name,
	   cfg->interrupt2.port->name,
	   cfg->interrupt2.pin);

	return 0;
}
