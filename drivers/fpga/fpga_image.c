/*
 * Copyright (c) 2026, Altera Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/fpga.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>

#include <bootutil/image.h>

int fpga_load_image(const struct device *dev, uint8_t area_id, uint32_t *buf, size_t buf_size)
{
	const struct flash_area *fa;
	struct image_header hdr;
	uint32_t hdr_size = 0U;
	uint32_t img_size = 0U;
	uint32_t flags;
	int ret;

	ret = flash_area_open(area_id, &fa);
	if (ret != 0) {
		return ret;
	}

	ret = flash_area_read(fa, 0, &hdr, sizeof(hdr));
	if (ret == 0) {
		/* All header fields are little-endian */
		hdr_size = sys_le16_to_cpu(hdr.ih_hdr_size);
		img_size = sys_le32_to_cpu(hdr.ih_img_size);
		flags = sys_le32_to_cpu(hdr.ih_flags);

		if ((sys_le32_to_cpu(hdr.ih_magic) != IMAGE_MAGIC) || (hdr_size < sizeof(hdr)) ||
		    (hdr_size > fa->fa_size) || (img_size > (fa->fa_size - hdr_size))) {
			ret = -ENOEXEC;
		} else if ((flags & (ENCRYPTIONFLAGS | COMPRESSIONFLAGS)) != 0U) {
			ret = -ENOTSUP;
		} else if (img_size > buf_size) {
			ret = -EFBIG;
		} else {
			ret = flash_area_read(fa, hdr_size, buf, img_size);
		}
	}

	flash_area_close(fa);

	if (ret != 0) {
		return ret;
	}

	return fpga_load(dev, buf, img_size);
}
