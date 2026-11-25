/*
 * Copyright (c) 2026, Altera Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/mgmt/fpga_mgmt.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <bootutil/image.h>

#include "fpga_mgmt_priv.h"

#if defined(CONFIG_FPGA_MGMT_BACKEND_FLASH_VERIFY_SHA256)
#include <psa/crypto.h>
#endif

LOG_MODULE_DECLARE(fpga_mgmt, CONFIG_FPGA_MGMT_LOG_LEVEL);

#define SHA256_SIZE 32U

#if defined(CONFIG_FPGA_MGMT_BACKEND_FLASH_VERIFY_SHA256)
/* Flash read buffer the image is hashed through */
static uint8_t read_buf[CONFIG_FPGA_MGMT_BACKEND_FLASH_HASH_BUF_SIZE];
#endif

struct image_layout {
	uint32_t hdr_size;
	uint32_t img_size;
	/* Header, bitstream and protected TLVs: the range the hash covers */
	uint32_t hashed_size;
};

static int header_read(const struct flash_area *fa, struct image_layout *layout)
{
	struct image_header hdr;
	uint32_t prot_size;
	int err;

	err = flash_area_read(fa, 0, &hdr, sizeof(hdr));
	if (err != 0) {
		LOG_ERR("Header read failed: %d", err);
		return err;
	}

	/* All header fields are little-endian */
	layout->hdr_size = sys_le16_to_cpu(hdr.ih_hdr_size);
	layout->img_size = sys_le32_to_cpu(hdr.ih_img_size);
	prot_size = sys_le16_to_cpu(hdr.ih_protect_tlv_size);

	LOG_DBG("Header: magic 0x%08x hdr %u img %u prot TLVs %u flags 0x%08x, area %zu bytes",
		sys_le32_to_cpu(hdr.ih_magic), layout->hdr_size, layout->img_size, prot_size,
		sys_le32_to_cpu(hdr.ih_flags), (size_t)fa->fa_size);

	if ((sys_le32_to_cpu(hdr.ih_magic) != IMAGE_MAGIC) || (layout->hdr_size < sizeof(hdr)) ||
	    (layout->img_size == 0U) || (layout->hdr_size > fa->fa_size) ||
	    (layout->img_size > (fa->fa_size - layout->hdr_size)) ||
	    (prot_size > (fa->fa_size - layout->hdr_size - layout->img_size))) {
		LOG_ERR("Bad magic or sizes");
		return -ENOEXEC;
	}

	/* The payload would not be a bitstream */
	if ((sys_le32_to_cpu(hdr.ih_flags) & (ENCRYPTIONFLAGS | COMPRESSIONFLAGS)) != 0U) {
		LOG_ERR("Encrypted or compressed image");
		return -ENOEXEC;
	}

	layout->hashed_size = layout->hdr_size + layout->img_size + prot_size;

	return 0;
}

#if defined(CONFIG_FPGA_MGMT_BACKEND_FLASH_VERIFY_SHA256)
/* Find the SHA-256 TLV in the unprotected TLV area and read its value */
static int sha256_tlv_read(const struct flash_area *fa, const struct image_layout *layout,
			   uint8_t *hash)
{
	struct image_tlv_info info;
	struct image_tlv tlv;
	uint32_t off = layout->hashed_size;
	uint32_t end;
	int err;

	if (sizeof(info) > (fa->fa_size - off)) {
		return -EBADMSG;
	}

	err = flash_area_read(fa, off, &info, sizeof(info));
	if (err != 0) {
		return err;
	}

	if ((sys_le16_to_cpu(info.it_magic) != IMAGE_TLV_INFO_MAGIC) ||
	    (sys_le16_to_cpu(info.it_tlv_tot) > (fa->fa_size - off))) {
		LOG_ERR("No TLV area at offset %u: magic 0x%04x size %u", off,
			sys_le16_to_cpu(info.it_magic), sys_le16_to_cpu(info.it_tlv_tot));
		return -EBADMSG;
	}

	LOG_DBG("TLV area at offset %u, %u bytes", off, sys_le16_to_cpu(info.it_tlv_tot));

	end = off + sys_le16_to_cpu(info.it_tlv_tot);
	off += sizeof(info);

	while ((end - off) >= sizeof(tlv)) {
		uint16_t len;

		err = flash_area_read(fa, off, &tlv, sizeof(tlv));
		if (err != 0) {
			return err;
		}
		off += sizeof(tlv);
		len = sys_le16_to_cpu(tlv.it_len);

		LOG_DBG("TLV type 0x%04x, %u bytes", sys_le16_to_cpu(tlv.it_type), len);

		if (len > (end - off)) {
			return -EBADMSG;
		}

		if (sys_le16_to_cpu(tlv.it_type) == IMAGE_TLV_SHA256) {
			if (len != SHA256_SIZE) {
				return -EBADMSG;
			}
			return flash_area_read(fa, off, hash, SHA256_SIZE);
		}

		off += len;
	}

	LOG_ERR("No SHA-256 TLV");

	return -EBADMSG;
}

static int sha256_verify(const struct flash_area *fa, const struct image_layout *layout)
{
	psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
	uint8_t expected[SHA256_SIZE];
	uint8_t actual[SHA256_SIZE];
	size_t actual_len;
	uint32_t off = 0U;
	int err;

	err = sha256_tlv_read(fa, layout, expected);
	if (err != 0) {
		return err;
	}

	if (psa_crypto_init() != PSA_SUCCESS) {
		return -EIO;
	}

	if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
		return -EIO;
	}

	while (off < layout->hashed_size) {
		size_t n = MIN(sizeof(read_buf), layout->hashed_size - off);

		err = flash_area_read(fa, off, read_buf, n);
		if (err != 0) {
			goto abort;
		}

		if (psa_hash_update(&op, read_buf, n) != PSA_SUCCESS) {
			err = -EIO;
			goto abort;
		}

		off += n;
	}

	if (psa_hash_finish(&op, actual, sizeof(actual), &actual_len) != PSA_SUCCESS) {
		err = -EIO;
		goto abort;
	}

	LOG_DBG("Hashed %u bytes", layout->hashed_size);
	LOG_HEXDUMP_DBG(expected, SHA256_SIZE, "SHA-256 TLV");
	LOG_HEXDUMP_DBG(actual, SHA256_SIZE, "SHA-256 of the image");

	return (memcmp(actual, expected, SHA256_SIZE) == 0) ? 0 : -EILSEQ;

abort:
	(void)psa_hash_abort(&op);
	return err;
}
#endif /* CONFIG_FPGA_MGMT_BACKEND_FLASH_VERIFY_SHA256 */

static int image_check(const struct flash_area *fa, struct image_layout *layout)
{
	int err;

	err = header_read(fa, layout);
	if (err != 0) {
		return err;
	}

#if defined(CONFIG_FPGA_MGMT_BACKEND_FLASH_VERIFY_SHA256)
	err = sha256_verify(fa, layout);
	if (err != 0) {
		return err;
	}
#endif

	return 0;
}

/* The core serializes the loads, so one open image is enough */
static struct {
	const struct flash_area *fa;
	uint32_t off;
} image;

static int flash_backend_open(const struct fpga_mgmt_source *src, void **ctx, size_t *size)
{
	struct image_layout layout;
	int err;

	err = flash_area_open(src->area_id, &image.fa);
	if (err != 0) {
		LOG_ERR("Flash area %u: open failed (%d)", src->area_id, err);
		return err;
	}

	err = image_check(image.fa, &layout);
	if (err != 0) {
		LOG_ERR("Flash area %u: invalid FPGA image (%d)", src->area_id, err);
		flash_area_close(image.fa);
		return err;
	}

	LOG_DBG("Flash area %u: bitstream at offset %u", src->area_id, layout.hdr_size);

	image.off = layout.hdr_size;
	*size = layout.img_size;
	*ctx = &image;

	return 0;
}

static int flash_backend_read(void *ctx, void *buf, size_t len)
{
	int err;

	ARG_UNUSED(ctx);

	err = flash_area_read(image.fa, image.off, buf, len);
	image.off += len;

	return err;
}

static void flash_backend_close(void *ctx)
{
	ARG_UNUSED(ctx);

	flash_area_close(image.fa);
}

const struct fpga_mgmt_backend_ops fpga_mgmt_flash_ops = {
	.open = flash_backend_open,
	.read = flash_backend_read,
	.close = flash_backend_close,
};
