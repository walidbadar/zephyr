/*
 * Copyright (c) 2026, Altera Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/fpga.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(fpga_bitstream_storage, LOG_LEVEL_INF);

/* fpga_load() takes the whole bitstream as an array of 32-bit words */
static uint32_t bitstream[DIV_ROUND_UP(CONFIG_SAMPLE_FPGA_BITSTREAM_MAX_SIZE, sizeof(uint32_t))];

#if defined(CONFIG_SAMPLE_FPGA_BITSTREAM_STORAGE_FLASH)

#include <zephyr/storage/flash_map.h>

#define BITSTREAM_SOURCE "the bitstream partition"

/* fpga_load_image() reads the bitstream behind the MCUboot image header and loads it */
static int bitstream_load(const struct device *fpga)
{
	return fpga_load_image(fpga, FIXED_PARTITION_ID(bitstream_partition), bitstream,
			       sizeof(bitstream));
}

#else /* File on the SD card */

#include <ff.h>
#include <zephyr/fs/fs.h>

#define MOUNT_POINT "/SD:"

#define BITSTREAM_SOURCE MOUNT_POINT "/" CONFIG_SAMPLE_FPGA_BITSTREAM_FILE_NAME

static FATFS fat_fs;

static struct fs_mount_t fs_mnt = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = MOUNT_POINT,
};

/* Mount the volume and list its root directory. */
static int setup_disk(void)
{
	struct fs_dirent entry;
	struct fs_dir_t dir;
	int ret;

	ret = fs_mount(&fs_mnt);
	if (ret != 0) {
		return ret;
	}

	fs_dir_t_init(&dir);

	ret = fs_opendir(&dir, MOUNT_POINT);
	if (ret != 0) {
		return ret;
	}

	LOG_INF("%s:", MOUNT_POINT);

	while (true) {
		ret = fs_readdir(&dir, &entry);
		if ((ret != 0) || (entry.name[0] == '\0')) {
			break;
		}

		LOG_INF("  %c %zu %s", (entry.type == FS_DIR_ENTRY_FILE) ? 'F' : 'D', entry.size,
			entry.name);
	}

	(void)fs_closedir(&dir);

	return ret;
}

static int bitstream_read(size_t *size)
{
	uint8_t *buf = (uint8_t *)bitstream;
	struct fs_dirent entry;
	struct fs_file_t file;
	size_t pos = 0;
	ssize_t len;
	int ret;

	ret = setup_disk();
	if (ret != 0) {
		return ret;
	}

	ret = fs_stat(BITSTREAM_SOURCE, &entry);
	if (ret != 0) {
		return ret;
	}

	if (entry.size > sizeof(bitstream)) {
		return -EFBIG;
	}

	fs_file_t_init(&file);

	ret = fs_open(&file, BITSTREAM_SOURCE, FS_O_READ);
	if (ret != 0) {
		return ret;
	}

	while (pos < entry.size) {
		len = fs_read(&file, &buf[pos], entry.size - pos);
		if (len <= 0) {
			ret = (len < 0) ? (int)len : -EIO;
			break;
		}

		pos += len;
	}

	(void)fs_close(&file);

	*size = pos;

	return ret;
}

static int bitstream_load(const struct device *fpga)
{
	size_t size;
	int ret;

	ret = bitstream_read(&size);
	if (ret != 0) {
		return ret;
	}

	LOG_INF("Read %zu bytes from %s", size, BITSTREAM_SOURCE);

	return fpga_load(fpga, bitstream, size);
}

#endif /* CONFIG_SAMPLE_FPGA_BITSTREAM_STORAGE_FLASH */

int main(void)
{
	const struct device *fpga = DEVICE_DT_GET(DT_ALIAS(fpga0));
	int ret;

	if (!device_is_ready(fpga)) {
		LOG_ERR("FPGA device %s is not ready", fpga->name);
		return 0;
	}

	ret = bitstream_load(fpga);
	if (ret != 0) {
		LOG_ERR("Failed to load the bitstream from %s (%d)", BITSTREAM_SOURCE, ret);
		return 0;
	}

	LOG_INF("Bitstream loaded, FPGA is %s",
		(fpga_get_status(fpga) == FPGA_STATUS_ACTIVE) ? "active" : "inactive");

	return 0;
}
