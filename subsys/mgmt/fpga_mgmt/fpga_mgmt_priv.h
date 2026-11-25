/*
 * Copyright (c) 2026, Altera Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SUBSYS_MGMT_FPGA_MGMT_FPGA_MGMT_PRIV_H_
#define ZEPHYR_SUBSYS_MGMT_FPGA_MGMT_FPGA_MGMT_PRIV_H_

#include <stddef.h>

#include <zephyr/mgmt/fpga_mgmt.h>

/**
 * @brief Operations of a bitstream storage backend
 *
 * The core calls them with its lock held, for one bitstream at a time, so a
 * backend can keep the state of the open bitstream in static data.
 */
struct fpga_mgmt_backend_ops {
	/**
	 * @brief Open and check the bitstream of a source
	 *
	 * @param src Bitstream source, with the backend of these operations.
	 * @param ctx Set to the context passed to the other operations.
	 * @param size Set to the bitstream size in bytes.
	 *
	 * @retval 0 The bitstream is open.
	 * @retval -errno Negative errno code on failure; nothing is left open.
	 */
	int (*open)(const struct fpga_mgmt_source *src, void **ctx, size_t *size);

	/**
	 * @brief Read the next bytes of the bitstream
	 *
	 * @param ctx Context set by open.
	 * @param buf Buffer for the data.
	 * @param len Number of bytes to read.
	 *
	 * @retval 0 All @p len bytes were read.
	 * @retval -errno Negative errno code on failure.
	 */
	int (*read)(void *ctx, void *buf, size_t len);

	/**
	 * @brief Close the bitstream
	 *
	 * @param ctx Context set by open.
	 */
	void (*close)(void *ctx);
};

/** Operations of the flash partition backend */
extern const struct fpga_mgmt_backend_ops fpga_mgmt_flash_ops;

/** Operations of the file system backend */
extern const struct fpga_mgmt_backend_ops fpga_mgmt_fs_ops;

#endif /* ZEPHYR_SUBSYS_MGMT_FPGA_MGMT_FPGA_MGMT_PRIV_H_ */
