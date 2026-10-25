/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Host (bottom) half of the native simulator host video source: a FIFO or a V4L2 device.
 *
 * This file is built in the native simulator runner context: it uses the host C
 * library and must not include any Zephyr header.
 */

/* For F_SETPIPE_SZ and F_GETPIPE_SZ */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <nsi_errno.h>
#include <nsi_tracing.h>

#include "video_native_sim_fifo_bottom.h"

/* Buffers queued on a V4L2 device, enough for the minimum some drivers need to stream */
#define VIDEO_NSI_V4L2_BUFFERS 4
/* V4L2 devices streaming at the same time, one per driver instance */
#define VIDEO_NSI_V4L2_DEVICES 4

struct video_nsi_v4l2_dev {
	int fd;
	/* Mapped buffers, zero for a free slot */
	unsigned int count;
	void *mem[VIDEO_NSI_V4L2_BUFFERS];
	size_t len[VIDEO_NSI_V4L2_BUFFERS];
	/* The device streams YUYV, converted on the host to the RGB565 requested */
	bool yuyv_to_rgb565;
};

static struct video_nsi_v4l2_dev video_nsi_v4l2_devs[VIDEO_NSI_V4L2_DEVICES];

/* The device streaming on fd, or a free slot when fd is -1 */
static struct video_nsi_v4l2_dev *video_nsi_v4l2_find(int fd)
{
	for (size_t i = 0; i < VIDEO_NSI_V4L2_DEVICES; i++) {
		struct video_nsi_v4l2_dev *dev = &video_nsi_v4l2_devs[i];

		if ((dev->count > 0) ? (dev->fd == fd) : (fd == -1)) {
			return dev;
		}
	}

	return NULL;
}

static int video_nsi_v4l2_capture(struct video_nsi_v4l2_dev *dev, uint8_t *buf, size_t size);

int video_nsi_fifo_default_path_bottom(const char *name, char *buf, size_t size)
{
	int len = snprintf(buf, size, "/tmp/zephyr-%s-%ld.fifo", name, (long)getpid());

	return ((len < 0) || ((size_t)len >= size)) ? -1 : 0;
}

void video_nsi_fifo_unlink_bottom(const char *path)
{
	(void)unlink(path);
}

int video_nsi_fifo_open_bottom(const char *path, bool *created, bool *v4l2)
{
	struct stat statbuf;
	int fd;
	int err;

	/*
	 * Create the FIFO unconditionally and validate what was actually opened
	 * afterwards: inspecting the path before opening it would leave a window for it
	 * to be replaced in between (time-of-check to time-of-use race).
	 */
	if (mkfifo(path, 0666) == 0) {
		*created = true;
	} else if (errno != EEXIST) {
		err = errno;
		nsi_print_warning("Could not create the FIFO %s (%s)\n", path, strerror(err));
		return -nsi_errno_to_mid(err);
	}

	/*
	 * Opening read-only and non-blocking succeeds even when no writer is attached,
	 * and guarantees that no later read() can stall the simulated system.
	 */
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		err = errno;
		nsi_print_warning("Could not open the FIFO %s (%s)\n", path, strerror(err));
		return -nsi_errno_to_mid(err);
	}

	if (fstat(fd, &statbuf) != 0) {
		err = errno;
		nsi_print_warning("Could not stat %s (%s)\n", path, strerror(err));
		(void)close(fd);
		return -nsi_errno_to_mid(err);
	}

	if (S_ISFIFO(statbuf.st_mode)) {
		*v4l2 = false;
		return fd;
	}

	if (!S_ISCHR(statbuf.st_mode)) {
		nsi_print_warning("%s exists but is neither a FIFO nor a character device, it "
				  "cannot be used as video source\n",
				  path);
		(void)close(fd);
		return -nsi_errno_to_mid(EEXIST);
	}

	/* The buffers of a V4L2 device can only be mapped from a read-write descriptor */
	(void)close(fd);
	fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		err = errno;
		nsi_print_warning("Could not open the V4L2 device %s (%s)\n", path, strerror(err));
		return -nsi_errno_to_mid(err);
	}

	*v4l2 = true;

	return fd;
}

int video_nsi_fifo_grow_pipe_bottom(int fd, size_t size)
{
	int capacity = fcntl(fd, F_GETPIPE_SZ);

	if (capacity < 0) {
		return -nsi_errno_to_mid(errno);
	}

	/*
	 * An unprivileged process gets EPERM above /proc/sys/fs/pipe-max-size, 1 MiB by
	 * default, or once its pipes together reach /proc/sys/fs/pipe-user-pages-soft.
	 * The kernel rounds the size up to a power of two, so halving the request,
	 * rounded up, tries each smaller capacity until one is allowed.
	 */
	for (size_t req = size; req > (size_t)capacity; req = (req + 1) / 2) {
		int ret = fcntl(fd, F_SETPIPE_SZ, (int)req);

		if (ret >= 0) {
			return ret;
		}

		if (errno != EPERM) {
			return -nsi_errno_to_mid(errno);
		}
	}

	return capacity;
}

int video_nsi_fifo_read_bottom(int fd, uint8_t *buf, size_t frame_size, size_t *offset)
{
	struct video_nsi_v4l2_dev *dev = video_nsi_v4l2_find(fd);

	if (dev != NULL) {
		return video_nsi_v4l2_capture(dev, buf, frame_size);
	}

	while (*offset < frame_size) {
		ssize_t ret = read(fd, &buf[*offset], frame_size - *offset);

		if (ret > 0) {
			*offset += (size_t)ret;
		} else if (ret == 0) {
			/* All the writers closed the FIFO */
			return -nsi_errno_to_mid(EPIPE);
		} else if (errno != EINTR) {
			return -nsi_errno_to_mid(errno);
		}
	}

	return 0;
}

static int video_nsi_v4l2_set_fmt(int fd, struct v4l2_format *fmt, uint32_t pixelformat,
				  uint32_t width, uint32_t height)
{
	memset(fmt, 0, sizeof(*fmt));
	fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt->fmt.pix.pixelformat = pixelformat;
	fmt->fmt.pix.width = width;
	fmt->fmt.pix.height = height;
	fmt->fmt.pix.field = V4L2_FIELD_NONE;

	if (ioctl(fd, VIDIOC_S_FMT, fmt) < 0) {
		int err = errno;

		nsi_print_warning("Could not set the format of the V4L2 device (%s)\n",
				  strerror(err));
		return -nsi_errno_to_mid(err);
	}

	return 0;
}

static void video_nsi_v4l2_stop(struct video_nsi_v4l2_dev *dev)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	struct v4l2_requestbuffers req = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};

	(void)ioctl(dev->fd, VIDIOC_STREAMOFF, &type);

	for (unsigned int i = 0; i < dev->count; i++) {
		(void)munmap(dev->mem[i], dev->len[i]);
	}

	/* Release the device buffers, so that another process can stream from it */
	(void)ioctl(dev->fd, VIDIOC_REQBUFS, &req);
	dev->count = 0;
}

int video_nsi_fifo_v4l2_start_bottom(int fd, uint32_t pixelformat, uint32_t width, uint32_t height,
				     uint32_t pitch)
{
	struct video_nsi_v4l2_dev *dev = video_nsi_v4l2_find(-1);
	struct v4l2_requestbuffers req = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
		.count = VIDEO_NSI_V4L2_BUFFERS,
	};
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	uint32_t devfmt = pixelformat;
	struct v4l2_format fmt;
	int err;

	if (dev == NULL) {
		nsi_print_warning("Too many V4L2 devices streaming at once\n");
		return -nsi_errno_to_mid(ENOMEM);
	}

	err = video_nsi_v4l2_set_fmt(fd, &fmt, devfmt, width, height);
	/* RGB565, the default format, is rarely provided by webcams: convert it from YUYV */
	if ((err == 0) && (fmt.fmt.pix.pixelformat != devfmt) && (devfmt == V4L2_PIX_FMT_RGB565)) {
		devfmt = V4L2_PIX_FMT_YUYV;
		err = video_nsi_v4l2_set_fmt(fd, &fmt, devfmt, width, height);
	}
	if (err < 0) {
		return err;
	}

	/* V4L2 adjusts a format it cannot provide instead of rejecting it */
	if ((fmt.fmt.pix.pixelformat != devfmt) || (fmt.fmt.pix.width != width) ||
	    (fmt.fmt.pix.height != height) || (fmt.fmt.pix.bytesperline != pitch)) {
		nsi_print_warning("The V4L2 device does not provide %ux%u %.4s with %u bytes per "
				  "line, it offers %ux%u %.4s with %u: see "
				  "v4l2-ctl --list-formats-ext\n",
				  width, height, (const char *)&pixelformat, pitch,
				  fmt.fmt.pix.width, fmt.fmt.pix.height,
				  (const char *)&fmt.fmt.pix.pixelformat, fmt.fmt.pix.bytesperline);
		return -nsi_errno_to_mid(ENOTSUP);
	}

	if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		err = errno;
		nsi_print_warning("The V4L2 device does not support memory mapped streaming (%s)\n",
				  strerror(err));
		return -nsi_errno_to_mid(err);
	}

	dev->fd = fd;
	dev->yuyv_to_rgb565 = (devfmt != pixelformat);

	/* The device may allocate more buffers than requested; the extra ones stay unused */
	for (unsigned int i = 0; (i < req.count) && (i < VIDEO_NSI_V4L2_BUFFERS); i++) {
		struct v4l2_buffer buf = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP,
			.index = i,
		};
		void *mem;

		if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
			err = errno;
			goto fail;
		}

		mem = mmap(NULL, buf.length, PROT_READ, MAP_SHARED, fd, buf.m.offset);
		if (mem == MAP_FAILED) {
			err = errno;
			goto fail;
		}

		dev->mem[i] = mem;
		dev->len[i] = buf.length;
		dev->count = i + 1;

		if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			err = errno;
			goto fail;
		}
	}

	if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		err = errno;
		goto fail;
	}

	return 0;

fail:
	nsi_print_warning("Could not start streaming from the V4L2 device (%s)\n", strerror(err));
	video_nsi_v4l2_stop(dev);
	return -nsi_errno_to_mid(err);
}

static uint8_t video_nsi_clamp8(int32_t v)
{
	return (v < 0) ? 0 : (v > 255) ? 255 : (uint8_t)v;
}

/* BT.601 limited range, two pixels per YUYV macropixel, RGB565 stored little-endian */
static void video_nsi_yuyv_to_rgb565(uint8_t *dst, const uint8_t *src, size_t len)
{
	for (size_t i = 0; (i + 3) < len; i += 4) {
		int32_t d = (int32_t)src[i + 1] - 128;
		int32_t e = (int32_t)src[i + 3] - 128;

		for (size_t p = 0; p < 2; p++) {
			int32_t c = 298 * ((int32_t)src[i + 2 * p] - 16) + 128;
			uint8_t r = video_nsi_clamp8((c + 409 * e) / 256);
			uint8_t g = video_nsi_clamp8((c - 100 * d - 208 * e) / 256);
			uint8_t b = video_nsi_clamp8((c + 516 * d) / 256);
			uint16_t rgb = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));

			dst[i + 2 * p] = (uint8_t)(rgb & 0xff);
			dst[i + 2 * p + 1] = (uint8_t)(rgb >> 8);
		}
	}
}

static int video_nsi_v4l2_capture(struct video_nsi_v4l2_dev *dev, uint8_t *buf, size_t size)
{
	struct v4l2_buffer vbuf = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};
	size_t len;

	/* The device is non-blocking: EAGAIN when no frame is ready */
	if (ioctl(dev->fd, VIDIOC_DQBUF, &vbuf) < 0) {
		return -nsi_errno_to_mid((errno == EINTR) ? EAGAIN : errno);
	}

	/* A frame the device flags as corrupted is skipped, like an empty one */
	len = ((vbuf.flags & V4L2_BUF_FLAG_ERROR) != 0) ? 0 : vbuf.bytesused;
	if (len > size) {
		len = size;
	}
	if (dev->yuyv_to_rgb565) {
		video_nsi_yuyv_to_rgb565(buf, dev->mem[vbuf.index], len);
	} else {
		memcpy(buf, dev->mem[vbuf.index], len);
	}

	if (ioctl(dev->fd, VIDIOC_QBUF, &vbuf) < 0) {
		return -nsi_errno_to_mid(errno);
	}

	return (len > 0) ? 0 : -nsi_errno_to_mid(EAGAIN);
}

void video_nsi_fifo_close_bottom(int fd)
{
	struct video_nsi_v4l2_dev *dev = video_nsi_v4l2_find(fd);

	if (dev != NULL) {
		video_nsi_v4l2_stop(dev);
	}

	(void)close(fd);
}
