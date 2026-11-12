/*
 * Copyright (c) 2021 Antmicro <www.antmicro.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup fpga_interface
 * @brief Main header file for FPGA driver API.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_FPGA_H_
#define ZEPHYR_INCLUDE_DRIVERS_FPGA_H_

#include <errno.h>
#include <stddef.h>

#include <zephyr/types.h>
#include <zephyr/sys/util.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Interfaces for Field-Programmable Gate Arrays (FPGA).
 * @defgroup fpga_interface FPGA
 * @since 2.7
 * @version 0.8.0
 * @ingroup io_interfaces
 * @{
 */

enum FPGA_status {
	/* Inactive is when the FPGA cannot accept the bitstream
	 * and will not be programmed correctly
	 */
	FPGA_STATUS_INACTIVE,
	/* Active is when the FPGA can accept the bitstream and
	 * can be programmed correctly
	 */
	FPGA_STATUS_ACTIVE
};

typedef enum FPGA_status (*fpga_api_get_status)(const struct device *dev);
typedef int (*fpga_api_load)(const struct device *dev, uint32_t *image_ptr,
			     uint32_t img_size);
typedef int (*fpga_api_reset)(const struct device *dev);
typedef int (*fpga_api_on)(const struct device *dev);
typedef int (*fpga_api_off)(const struct device *dev);
typedef const char *(*fpga_api_get_info)(const struct device *dev);

__subsystem struct fpga_driver_api {
	fpga_api_get_status get_status;
	fpga_api_reset reset;
	fpga_api_load load;
	fpga_api_on on;
	fpga_api_off off;
	fpga_api_get_info get_info;
};

/**
 * @brief Read the status of FPGA.
 *
 * @param dev FPGA device structure.
 *
 * @retval 0 if the FPGA is in INACTIVE state.
 * @retval 1 if the FPGA is in ACTIVE state.
 */
static inline enum FPGA_status fpga_get_status(const struct device *dev)
{
	const struct fpga_driver_api *api =
		(const struct fpga_driver_api *)dev->api;

	if (api->get_status == NULL) {
		/* assume it can never be reprogrammed if it
		 * doesn't support the get_status callback
		 */
		return FPGA_STATUS_INACTIVE;
	}

	return api->get_status(dev);
}

/**
 * @brief Reset the FPGA.
 *
 * @param dev FPGA device structure.
 *
 * @retval 0 if successful.
 * @retval Failed Otherwise.
 */
static inline int fpga_reset(const struct device *dev)
{
	const struct fpga_driver_api *api =
		(const struct fpga_driver_api *)dev->api;

	if (api->reset == NULL) {
		return -ENOTSUP;
	}

	return api->reset(dev);
}

/**
 * @brief Load the bitstream and program the FPGA
 *
 * @param dev FPGA device structure.
 * @param image_ptr Pointer to bitstream.
 * @param img_size Bitstream size in bytes.
 *
 * @retval 0 if successful.
 * @retval Failed Otherwise.
 */
static inline int fpga_load(const struct device *dev, uint32_t *image_ptr,
			    uint32_t img_size)
{
	const struct fpga_driver_api *api =
		(const struct fpga_driver_api *)dev->api;

	if (api->load == NULL) {
		return -ENOTSUP;
	}

	return api->load(dev, image_ptr, img_size);
}

/**
 * @brief Load a bitstream stored as an MCUboot image and program the FPGA
 *
 * The flash area starts with an MCUboot image header, as written by imgtool, followed by the
 * bitstream. The header gives the offset and the size of the bitstream, which is read into
 * @p buf and passed to fpga_load(). The image hash and signature are not verified.
 *
 * Available with @kconfig{CONFIG_FPGA_LOAD_IMAGE}.
 *
 * @param dev FPGA device structure.
 * @param area_id ID of the flash area that holds the image.
 * @param buf Buffer the bitstream is read into.
 * @param buf_size Size of @p buf in bytes.
 *
 * @retval 0 Success.
 * @retval -ENOEXEC No valid MCUboot image header.
 * @retval -ENOEXEC The image does not fit in the flash area.
 * @retval -ENOTSUP The image is encrypted or compressed.
 * @retval -ENOTSUP The driver does not implement fpga_load().
 * @retval -EFBIG The bitstream is larger than @p buf_size.
 * @retval -errno Other negative errno code from the flash area or from fpga_load().
 */
int fpga_load_image(const struct device *dev, uint8_t area_id, uint32_t *buf, size_t buf_size);

/**
 * @brief Turns on the FPGA.
 *
 * @param dev FPGA device structure.
 *
 * @retval 0 if successful.
 * @retval negative errno code on failure.
 */
static inline int fpga_on(const struct device *dev)
{
	const struct fpga_driver_api *api =
		(const struct fpga_driver_api *)dev->api;

	if (api->on == NULL) {
		return -ENOTSUP;
	}

	return api->on(dev);
}

#define FPGA_GET_INFO_DEFAULT "n/a"

/**
 * @brief Returns information about the FPGA.
 *
 * @param dev FPGA device structure.
 *
 * @return String containing information.
 */
static inline const char *fpga_get_info(const struct device *dev)
{
	const struct fpga_driver_api *api =
		(const struct fpga_driver_api *)dev->api;

	if (api->get_info == NULL) {
		return FPGA_GET_INFO_DEFAULT;
	}

	return api->get_info(dev);
}

/**
 * @brief Turns off the FPGA.
 *
 * @param dev FPGA device structure.
 *
 * @retval 0 if successful.
 * @retval negative errno code on failure.
 */
static inline int fpga_off(const struct device *dev)
{
	const struct fpga_driver_api *api =
		(const struct fpga_driver_api *)dev->api;

	if (api->off == NULL) {
		return -ENOTSUP;
	}

	return api->off(dev);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_FPGA_H_ */
