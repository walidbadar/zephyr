/*
 * Copyright (c) 2026, Altera Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup fpga_mgmt
 * @brief Load FPGA bitstreams from flash partitions and file systems.
 */

#ifndef ZEPHYR_INCLUDE_MGMT_FPGA_MGMT_H_
#define ZEPHYR_INCLUDE_MGMT_FPGA_MGMT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FPGA bitstream management
 * @defgroup fpga_mgmt FPGA bitstream management
 * @ingroup os_services
 * @{
 */

/** Storage backends a bitstream can be loaded from */
enum fpga_mgmt_backend {
	/**
	 * Flash partition, with CONFIG_FPGA_MGMT_BACKEND_FLASH. The bitstream
	 * is the payload of an MCUboot image, as written by imgtool, so the
	 * partition needs no file system to record its size. The image
	 * header and, with CONFIG_FPGA_MGMT_BACKEND_FLASH_VERIFY_SHA256, the
	 * SHA-256 hash TLV are checked; signatures are not verified.
	 */
	FPGA_MGMT_BACKEND_FLASH,
	/**
	 * File on a mounted file system, with CONFIG_FPGA_MGMT_BACKEND_FS.
	 * The file holds the bitstream as is.
	 */
	FPGA_MGMT_BACKEND_FS,
};

/** Bitstream source: a backend and where the bitstream is in it */
struct fpga_mgmt_source {
	/** Backend that reads the bitstream */
	enum fpga_mgmt_backend backend;
	union {
		/** Flash area ID, for FPGA_MGMT_BACKEND_FLASH */
		uint8_t area_id;
		/** Absolute path of the bitstream file, for FPGA_MGMT_BACKEND_FS */
		const char *path;
	};
};

/**
 * @brief Load a bitstream from storage
 *
 * The backend of @p src opens and checks the bitstream, which is then passed
 * to the device of the zephyr,fpga chosen node with fpga_load(), in chunks of
 * CONFIG_FPGA_LOAD_CHUNK_SIZE bytes. Returns once the load has finished.
 *
 * @param src Bitstream source.
 *
 * @retval 0 The FPGA is configured.
 * @retval -ENODEV The FPGA device is not ready.
 * @retval -ENOTSUP The backend is not enabled.
 * @retval -ENOEXEC No valid image header, an encrypted or compressed image,
 *         or not a bitstream file.
 * @retval -EBADMSG No valid TLV area or no SHA-256 TLV in the image.
 * @retval -EILSEQ Image hash mismatch.
 * @retval -ENOENT No such file.
 * @retval -errno Other negative errno code from the storage or from the
 *         FPGA driver.
 */
int fpga_mgmt_load(const struct fpga_mgmt_source *src);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MGMT_FPGA_MGMT_H_ */
