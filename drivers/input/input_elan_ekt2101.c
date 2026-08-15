/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT elan_ekt2101

#include <zephyr/input/input.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(elan_ekt2101, CONFIG_INPUT_LOG_LEVEL);

/* All transactions are fixed 4-byte (32-bit) packets. */
#define EKT2101_PACKET_LEN 4

/* Byte 0 (Type + PID) values, see datasheet section 11. */
#define EKT2101_PID_BTN_STATUS 0x52 /* Packet ID #2, device -> host, reply to a read command */
#define EKT2101_PID_BTN_READ   0x53 /* Packet ID #3, host -> device, read command */
#define EKT2101_PID_HELLO      0x55 /* Packet Hello, device -> host, on power-up */

#define EKT2101_REG_BTN_STATUS 1
#define EKT2101_NUM_BUTTONS    18

#define EKT2101_REPLY_TIMEOUT_MS 100

struct ekt2101_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec int_gpio;
	uint32_t polling_interval_ms;
};

struct ekt2101_data {
	const struct device *dev;
	struct k_sem reply;
	struct k_work_delayable work;
	struct gpio_callback int_gpio_cb;

	/* Last known button bitmap, bit (n-1) == Btn(n), used for edge detect. */
	uint32_t btn_state;
};

/*
 * Decode the button bitmap (bits 19..2 of the 32-bit word) and report any
 * button whose state changed since the last packet.
 *
 *   pkt[1] low nibble : Btn1..Btn4  (bits 19..16)
 *   pkt[2]             : Btn5..Btn12 (bits 15..8)
 *   pkt[3] bits 7..2   : Btn13..Btn18 (bits 7..2)
 */
static void ekt2101_report_buttons(const struct device *dev, const uint8_t *pkt)
{
	struct ekt2101_data *data = dev->data;
	uint32_t packed_btn;
	uint32_t new_state = 0;
	uint32_t changed;

	packed_btn = ((uint32_t)(pkt[1] & 0x0F) << 14) | ((uint32_t)pkt[2] << 6) | (pkt[3] >> 2);
	LOG_DBG("raw=%02x %02x %02x %02x packed=0x%05x", pkt[0], pkt[1], pkt[2], pkt[3],
		packed_btn);

	for (int btn = 1; btn <= EKT2101_NUM_BUTTONS; btn++) {
		if (packed_btn & BIT(EKT2101_NUM_BUTTONS - btn)) {
			new_state |= BIT(btn - 1);
		}
	}

	changed = new_state ^ data->btn_state;
	data->btn_state = new_state;

	for (int btn = 1; btn <= EKT2101_NUM_BUTTONS; btn++) {
		if (changed & BIT(btn - 1)) {
			bool pressed = (new_state & BIT(btn - 1)) != 0;

			/* NOTE: maps directly to INPUT_BTN_0.._17; remap in the
			 * application if a specific keycode layout is needed.
			 */
			input_report_key(dev, INPUT_BTN_0 + (btn - 1), pressed, true, K_FOREVER);
		}
	}
}

static void ekt2101_handle_packet(const struct device *dev, const uint8_t *pkt)
{
	switch (pkt[0]) {
	case EKT2101_PID_BTN_STATUS:
		ekt2101_report_buttons(dev, pkt);
		break;
	case EKT2101_PID_HELLO:
		LOG_INF("eKT2101 hello packet, device ready");
		break;
	default:
		LOG_WRN("Unexpected packet id 0x%02x", pkt[0]);
		break;
	}
}

static void ekt2101_isr_handler(const struct device *dev, struct gpio_callback *cb, uint32_t mask)
{
	struct ekt2101_data *data = CONTAINER_OF(cb, struct ekt2101_data, int_gpio_cb);

	ARG_UNUSED(dev);
	ARG_UNUSED(mask);

	k_sem_give(&data->reply);
}

static void ekt2101_write_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct ekt2101_data *data = CONTAINER_OF(dwork, struct ekt2101_data, work);
	const struct device *dev = data->dev;
	const struct ekt2101_config *cfg = dev->config;
	uint8_t pkt[EKT2101_PACKET_LEN];
	int ret;

	pkt[0] = EKT2101_PID_BTN_READ;
	pkt[1] = (EKT2101_REG_BTN_STATUS & 0x0F) << 4;
	pkt[2] = 0x00;
	pkt[3] = 0x01;

	ret = i2c_write_dt(&cfg->i2c, pkt, sizeof(pkt));
	if (ret < 0) {
		LOG_ERR("Could not write packet: %d", ret);
		goto reschedule;
	}

	LOG_DBG("write raw=%02x %02x %02x %02x", pkt[0], pkt[1], pkt[2], pkt[3]);

	ret = k_sem_take(&data->reply, K_MSEC(cfg->polling_interval_ms));
	if (ret < 0) {
		LOG_WRN("Timed out waiting for TPreqB reply");
		goto reschedule;
	}

	k_usleep(50);

	ret = i2c_read_dt(&cfg->i2c, pkt, sizeof(pkt));
	if (ret < 0) {
		LOG_ERR("Could not read packet: %d", ret);
		goto reschedule;
	}

	LOG_DBG("read raw=%02x %02x %02x %02x", pkt[0], pkt[1], pkt[2], pkt[3]);

	ekt2101_handle_packet(dev, pkt);

reschedule:
	k_work_reschedule(dwork, K_MSEC(cfg->polling_interval_ms));
}

static int ekt2101_init(const struct device *dev)
{
	const struct ekt2101_config *cfg = dev->config;
	struct ekt2101_data *data = dev->data;
	int ret;

	data->dev = dev;
	k_sem_init(&data->reply, 0, 1);

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR_DEVICE_NOT_READY(cfg->i2c.bus);
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&cfg->int_gpio)) {
		LOG_ERR_DEVICE_NOT_READY(cfg->int_gpio.port);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->int_gpio, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Could not configure interrupt GPIO pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&cfg->int_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Could not configure interrupt GPIO interrupt: %d", ret);
		return ret;
	}

	gpio_init_callback(&data->int_gpio_cb, ekt2101_isr_handler, BIT(cfg->int_gpio.pin));

	ret = gpio_add_callback(cfg->int_gpio.port, &data->int_gpio_cb);
	if (ret < 0) {
		LOG_ERR("Could not set gpio callback: %d", ret);
		return ret;
	}

	k_work_init_delayable(&data->work, ekt2101_write_handler);
	k_work_reschedule(&data->work, K_MSEC(cfg->polling_interval_ms));

	return 0;
}

#define EKT2101_INST(inst)                                                                         \
	static const struct ekt2101_config ekt2101_config_##inst = {                               \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.int_gpio = GPIO_DT_SPEC_INST_GET(inst, irq_gpios),                                \
		.polling_interval_ms = DT_INST_PROP(inst, polling_interval_ms),                    \
	};                                                                                         \
                                                                                                   \
	static struct ekt2101_data ekt2101_data_##inst;                                            \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, &ekt2101_init, NULL, &ekt2101_data_##inst,                     \
			      &ekt2101_config_##inst, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY,     \
			      NULL);

DT_INST_FOREACH_STATUS_OKAY(EKT2101_INST)
