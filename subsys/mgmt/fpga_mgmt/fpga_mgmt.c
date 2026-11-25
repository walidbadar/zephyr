/*
 * Copyright (c) 2026, Altera Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/fpga.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/fpga_mgmt.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#include "fpga_mgmt_priv.h"

LOG_MODULE_REGISTER(fpga_mgmt, CONFIG_FPGA_MGMT_LOG_LEVEL);

/* The FPGA all bitstreams are loaded into */
static const struct device *const fpga_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_fpga));

/* fpga_load() takes its data as an array of 32-bit words */
static uint32_t chunk[CONFIG_FPGA_LOAD_CHUNK_SIZE / sizeof(uint32_t)];

/* Serializes the loads: the chunk buffer and the state of the backends */
static K_MUTEX_DEFINE(load_lock);

BUILD_ASSERT((CONFIG_FPGA_LOAD_CHUNK_SIZE % sizeof(uint32_t)) == 0,
	     "The chunk size must be a multiple of 4");

static const struct fpga_mgmt_backend_ops *backend_ops(enum fpga_mgmt_backend backend)
{
	switch (backend) {
#if defined(CONFIG_FPGA_MGMT_BACKEND_FLASH)
	case FPGA_MGMT_BACKEND_FLASH:
		return &fpga_mgmt_flash_ops;
#endif
#if defined(CONFIG_FPGA_MGMT_BACKEND_FS)
	case FPGA_MGMT_BACKEND_FS:
		return &fpga_mgmt_fs_ops;
#endif
	default:
		return NULL;
	}
}

/* Read the bitstream in chunks and pass each one to fpga_load() */
static int load_chunked(const struct fpga_mgmt_backend_ops *ops, void *ctx, size_t size)
{
	/* The driver has taken a full chunk and expects the rest of the bitstream */
	bool open = false;
	size_t off = 0;
	int err;

	while (true) {
		size_t len = MIN(size - off, sizeof(chunk));

		if (len > 0U) {
			err = ops->read(ctx, chunk, len);
			if (err != 0) {
				LOG_ERR("Bitstream read failed at offset %zu: %d", off, err);
				if (open) {
					/* An empty chunk ends the bitstream in the driver */
					(void)fpga_load(fpga_dev, chunk, 0U);
				}
				break;
			}
		}

		/* A chunk shorter than the buffer, which can be empty, is the last one */
		err = fpga_load(fpga_dev, chunk, len);
		if ((err != 0) || (len < sizeof(chunk))) {
			LOG_DBG("fpga_load() returned %d after %zu bytes", err, off + len);
			break;
		}

		off += len;
		open = true;
	}

	return err;
}

int fpga_mgmt_load(const struct fpga_mgmt_source *src)
{
	const struct fpga_mgmt_backend_ops *ops = backend_ops(src->backend);
	size_t size;
	void *ctx;
	int err;

	if (!device_is_ready(fpga_dev)) {
		LOG_ERR("%s is not ready", fpga_dev->name);
		return -ENODEV;
	}

	if (ops == NULL) {
		LOG_ERR("Backend %d is not enabled", src->backend);
		return -ENOTSUP;
	}

	(void)k_mutex_lock(&load_lock, K_FOREVER);

	err = ops->open(src, &ctx, &size);
	if (err == 0) {
		LOG_INF("%s: loading %zu bytes", fpga_dev->name, size);

		err = load_chunked(ops, ctx, size);
		ops->close(ctx);
	}

	k_mutex_unlock(&load_lock);

	return err;
}

#if defined(CONFIG_FPGA_MGMT_LOAD_ON_BOOT)
static int fpga_mgmt_boot_load(void)
{
	const struct fpga_mgmt_source src = {
#if defined(CONFIG_FPGA_MGMT_LOAD_ON_BOOT_FS)
		.backend = FPGA_MGMT_BACKEND_FS,
		.path = CONFIG_FPGA_MGMT_BACKEND_FS_BOOT_FILE,
#elif defined(CONFIG_FPGA_MGMT_LOAD_ON_BOOT_FLASH)
		.backend = FPGA_MGMT_BACKEND_FLASH,
		.area_id = DT_FIXED_PARTITION_ID(DT_CHOSEN(zephyr_fpga_bitstream_partition)),
#endif
	};
	int err;

	err = fpga_mgmt_load(&src);
	if (err != 0) {
		/* The system boots with the FPGA left as it was */
		LOG_ERR("%s: no bitstream loaded at boot (%d)", fpga_dev->name, err);
	} else {
		LOG_INF("%s configured at boot", fpga_dev->name);
	}

	return 0;
}

SYS_INIT(fpga_mgmt_boot_load, APPLICATION, CONFIG_FPGA_MGMT_LOAD_ON_BOOT_PRIORITY);
#endif /* CONFIG_FPGA_MGMT_LOAD_ON_BOOT */
