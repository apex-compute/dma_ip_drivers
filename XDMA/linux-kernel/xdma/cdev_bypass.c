/*
 * This file is part of the Xilinx DMA IP Core driver for Linux
 *
 * Copyright (c) 2016-present,  Xilinx, Inc.
 * All rights reserved.
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 */
#define pr_fmt(fmt)	KBUILD_MODNAME ":%s: " fmt, __func__

#include "libxdma_api.h"
#include "xdma_cdev.h"

#define write_register(v, mem, off) iowrite32(v, mem)

static int copy_desc_data(struct xdma_transfer *transfer, u8 *buf,
		size_t *buf_offset, size_t buf_size)
{
	int i;
	int rc = 0;

	if (!buf) {
		pr_err("Invalid buffer\n");
		return -EINVAL;
	}

	if (!buf_offset) {
		pr_err("Invalid buffer offset\n");
		return -EINVAL;
	}

	/*
	 * Fill buffer with descriptor data as CPU-native 32-bit words,
	 * the same format char_bypass_write() takes.
	 */
	for (i = 0; i < transfer->desc_num; i++) {
		if (*buf_offset + sizeof(struct xdma_desc) <= buf_size) {
			__le32 *src = (__le32 *)(transfer->desc_virt + i);
			u32 words[sizeof(struct xdma_desc) / sizeof(u32)];
			int j;

			for (j = 0; j < ARRAY_SIZE(words); j++)
				words[j] = le32_to_cpu(src[j]);

			memcpy(&buf[*buf_offset], words, sizeof(words));
			*buf_offset += sizeof(struct xdma_desc);
		} else {
			rc = -ENOMEM;
		}
	}

	return rc;
}

static ssize_t char_bypass_read(struct file *file, char __user *buf,
		size_t count, loff_t *pos)
{
	struct xdma_dev *xdev;
	struct xdma_engine *engine;
	struct xdma_cdev *xcdev = (struct xdma_cdev *)file->private_data;
	struct xdma_transfer *transfer;
	struct list_head *idx;
	size_t buf_offset = 0;
	size_t buf_size;
	u8 *kbuf;
	int rc = 0;

	rc = xcdev_check(__func__, xcdev, 1);
	if (rc < 0)
		return rc;
	xdev = xcdev->xdev;
	engine = xcdev->engine;

	dbg_sg("In %s()\n", __func__);

	if (count & 3) {
		dbg_sg("Buffer size must be a multiple of 4 bytes\n");
		return -EINVAL;
	}

	if (!buf) {
		dbg_sg("Caught NULL pointer\n");
		return -EINVAL;
	}

	if (xdev->bypass_bar_idx < 0) {
		dbg_sg("Bypass BAR not present - unsupported operation\n");
		return -ENODEV;
	}

	/*
	 * Queued transfers all live in the engine descriptor ring, so the
	 * ring size bounds what can be returned. Stage the data in a kernel
	 * buffer: copy_to_user() may sleep and must not be under the lock.
	 */
	buf_size = min_t(size_t, count,
			 engine->desc_max * sizeof(struct xdma_desc));
	kbuf = kmalloc(buf_size, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	spin_lock(&engine->lock);

	if (!list_empty(&engine->transfer_list)) {
		list_for_each(idx, &engine->transfer_list) {
			transfer = list_entry(idx, struct xdma_transfer, entry);

			rc = copy_desc_data(transfer, kbuf, &buf_offset,
					    buf_size);
		}
	}

	spin_unlock(&engine->lock);

	if (rc >= 0 && copy_to_user(buf, kbuf, buf_offset)) {
		dbg_sg("Copy to user buffer failed\n");
		rc = -EFAULT;
	}
	kfree(kbuf);

	if (rc < 0)
		return rc;
	else
		return buf_offset;
}

static ssize_t char_bypass_write(struct file *file, const char __user *buf,
		size_t count, loff_t *pos)
{
	struct xdma_dev *xdev;
	struct xdma_engine *engine;
	struct xdma_cdev *xcdev = (struct xdma_cdev *)file->private_data;

	u32 desc_data[64];	/* a multiple of whole descriptors */
	void __iomem *bypass_addr;
	size_t buf_offset = 0;
	int rc = 0;
	int copy_err;

	rc = xcdev_check(__func__, xcdev, 1);
	if (rc < 0)
		return rc;
	xdev = xcdev->xdev;
	engine = xcdev->engine;

	if (count & 3) {
		dbg_sg("Buffer size must be a multiple of 4 bytes\n");
		return -EINVAL;
	}

	if (!buf) {
		dbg_sg("Caught NULL pointer\n");
		return -EINVAL;
	}

	if (xdev->bypass_bar_idx < 0) {
		dbg_sg("Bypass BAR not present - unsupported operation\n");
		return -ENODEV;
	}

	dbg_sg("In %s()\n", __func__);

	/* Write descriptor data to the bypass BAR */
	bypass_addr = xdev->bar[xdev->bypass_bar_idx];
	bypass_addr = (void __iomem *)(
			(u32 __iomem *)bypass_addr + engine->bypass_offset
			);
	while (buf_offset < count) {
		size_t len = min_t(size_t, count - buf_offset,
				   sizeof(desc_data));
		size_t i;

		/* copy_from_user() may sleep, so not under the spinlock */
		copy_err = copy_from_user(desc_data, &buf[buf_offset], len);
		if (copy_err) {
			dbg_sg("Error reading data from userspace buffer\n");
			rc = -EFAULT;
			break;
		}

		spin_lock(&engine->lock);
		for (i = 0; i < len / sizeof(u32); i++)
			write_register(desc_data[i], bypass_addr,
					bypass_addr - engine->bypass_offset);
		spin_unlock(&engine->lock);

		buf_offset += len;
		rc = buf_offset;
	}

	return rc;
}


/*
 * character device file operations for bypass operation
 */

static const struct file_operations bypass_fops = {
	.owner = THIS_MODULE,
	.open = char_open,
	.release = char_close,
	.read = char_bypass_read,
	.write = char_bypass_write,
	.mmap = bridge_mmap,
};

void cdev_bypass_init(struct xdma_cdev *xcdev)
{
	cdev_init(&xcdev->cdev, &bypass_fops);
}
