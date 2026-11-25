/*
 * Copyright (c) 2026, Altera Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/fpga_mgmt.h>
#include <zephyr/sys/util.h>

#include "fpga_mgmt_priv.h"

LOG_MODULE_DECLARE(fpga_mgmt, CONFIG_FPGA_MGMT_LOG_LEVEL);

/* The core serializes the loads, so one open file is enough */
static struct fs_file_t file;

static int fs_backend_open(const struct fpga_mgmt_source *src, void **ctx, size_t *size)
{
	struct fs_dirent entry;
	int err;

	err = fs_stat(src->path, &entry);
	if (err != 0) {
		LOG_ERR("%s: no bitstream file (%d)", src->path, err);
		return err;
	}

	if ((entry.type != FS_DIR_ENTRY_FILE) || (entry.size == 0U)) {
		LOG_ERR("%s: not a bitstream file", src->path);
		return -ENOEXEC;
	}

	fs_file_t_init(&file);

	err = fs_open(&file, src->path, FS_O_READ);
	if (err != 0) {
		LOG_ERR("%s: open failed (%d)", src->path, err);
		return err;
	}

	*size = entry.size;
	*ctx = &file;

	return 0;
}

static int fs_backend_read(void *ctx, void *buf, size_t len)
{
	uint8_t *dst = buf;
	size_t pos = 0;

	ARG_UNUSED(ctx);

	while (pos < len) {
		ssize_t n = fs_read(&file, &dst[pos], len - pos);

		if (n < 0) {
			return (int)n;
		}

		if (n == 0) {
			return -EIO;
		}

		pos += (size_t)n;
	}

	return 0;
}

static void fs_backend_close(void *ctx)
{
	ARG_UNUSED(ctx);

	(void)fs_close(&file);
}

const struct fpga_mgmt_backend_ops fpga_mgmt_fs_ops = {
	.open = fs_backend_open,
	.read = fs_backend_read,
	.close = fs_backend_close,
};
