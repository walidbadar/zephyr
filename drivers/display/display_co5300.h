/*
 * Copyright (c) 2026 VIEWE Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_DISPLAY_DISPLAY_CO5300_H_
#define ZEPHYR_DRIVERS_DISPLAY_DISPLAY_CO5300_H_

#if DT_ANY_INST_ON_BUS_STATUS_OKAY(mipi_dsi)
#include <zephyr/drivers/mipi_dsi.h>
#include <zephyr/drivers/mipi_dsi/mipi_dsi_mcux_2l.h>
#include <fsl_lcdif.h>
#include <fsl_mipi_dsi.h>

#define CO5300_PIXFMT_RGB565 MIPI_DSI_PIXFMT_RGB565
#define CO5300_PIXFMT_RGB888 MIPI_DSI_PIXFMT_RGB888
#endif

#if DT_ANY_INST_ON_BUS_STATUS_OKAY(spi)
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>

#define CO5300_PIXFMT_RGB565 PIXEL_FORMAT_RGB_565
#define CO5300_PIXFMT_RGB888 PIXEL_FORMAT_RGB_888

#define CO5300_MSPI_OPCODE_WRITE_CMD   (0x02U)
#define CO5300_QSPI_OPCODE_WRITE_COLOR (0x32U)
#endif

typedef int (*co5300_cmd_write_fn)(const struct device *dev, uint8_t cmd, uint8_t *data,
				   size_t length);
typedef int (*co5300_display_transfer_fn)(const struct device *dev,
					  struct display_buffer_descriptor *local_desc,
					  const uint8_t *tx_buf, uint32_t tx_len);

/* display command structure passed to mipi to control the display */
struct display_cmds {
	uint8_t *cmd_code;
	uint8_t size;
};

struct co5300_config {
	const struct device *mipi_dev;
	co5300_cmd_write_fn cmd_write;
	co5300_display_transfer_fn display_transfer;
#if DT_ANY_INST_ON_BUS_STATUS_OKAY(spi)
	struct spi_config mspi_config;
#endif
#if DT_ANY_INST_ON_BUS_STATUS_OKAY(mipi_dsi)
	uint16_t channel;
	uint16_t num_of_lanes;
#endif
	const struct gpio_dt_spec reset_gpios;
	const struct gpio_dt_spec tear_effect_gpios;
	const struct gpio_dt_spec backlight_gpios;
	uint16_t panel_width;
	uint16_t panel_height;
};

struct co5300_data {
	uint8_t pixel_format;
	uint8_t bytes_per_pixel;
	struct gpio_callback tear_effect_gpio_cb;
	struct k_sem tear_effect_sem;
	/* Pointer to framebuffer */
	uint8_t *frame_ptr;
	uint32_t frame_pitch;
};

#endif /* ZEPHYR_DRIVERS_DISPLAY_DISPLAY_CO5300_H_ */
