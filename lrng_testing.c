// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * Linux Random Number Generator (LRNG) testing interfaces
 *
 * Copyright (C) 2019 - 2020, Stephan Mueller <smueller@chronox.de>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <asm/errno.h>

#include "lrng_internal.h"

#define LRNG_TESTING_RINGBUFFER_SIZE	1024
#define LRNG_TESTING_RINGBUFFER_MASK	(LRNG_TESTING_RINGBUFFER_SIZE - 1)

struct lrng_testing {
	u32 lrng_testing_rb[LRNG_TESTING_RINGBUFFER_SIZE];
	u32 rb_reader;
	u32 rb_writer;
	atomic_t lrng_testing_enabled;
	spinlock_t lock;
	wait_queue_head_t read_wait;
};

/*************************** Generic Data Handling ****************************/

/*
 * boot variable:
 * 0 ==> No boot test, gathering of runtime data allowed
 * 1 ==> Boot test enabled and ready for collecting data, gathering runtime
 *	 data is disabled
 * 2 ==> Boot test completed and disabled, gathering of runtime data is
 *	 disabled
 */

static inline void lrng_testing_reset(struct lrng_testing *data)
{
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);
	data->rb_reader = 0;
	data->rb_writer = 0;
	spin_unlock_irqrestore(&data->lock, flags);
}

static inline void lrng_testing_init(struct lrng_testing *data, u32 boot)
{
	/*
	 * The boot time testing implies we have a running test. If the
	 * caller wants to clear it, he has to unset the boot_test flag
	 * at runtime via sysfs to enable regular runtime testing
	 */
	if (boot)
		return;

	lrng_testing_reset(data);
	atomic_set(&data->lrng_testing_enabled, 1);
	pr_warn("Enabling data collection\n");
}

static inline void lrng_testing_fini(struct lrng_testing *data)
{
	atomic_set(&data->lrng_testing_enabled, 0);
	lrng_testing_reset(data);
	pr_warn("Disabling data collection\n");
}


static inline bool lrng_testing_store(struct lrng_testing *data, u32 value,
				      u32 *boot)
{
	unsigned long flags;

	if (!atomic_read(&data->lrng_testing_enabled) && (*boot != 1))
		return false;

	spin_lock_irqsave(&data->lock, flags);

	/*
	 * Disable entropy testing for boot time testing after ring buffer
	 * is filled.
	 */
	if (*boot) {
		if (data->rb_writer > LRNG_TESTING_RINGBUFFER_SIZE) {
			*boot = 2;
			pr_warn_once("One time data collection test disabled\n");
			spin_unlock_irqrestore(&data->lock, flags);
			return false;
		}

		if (data->rb_writer == 1)
			pr_warn("One time data collection test enabled\n");
	}

	data->lrng_testing_rb[data->rb_writer & LRNG_TESTING_RINGBUFFER_MASK] =
									value;
	data->rb_writer++;

	spin_unlock_irqrestore(&data->lock, flags);

	if (wq_has_sleeper(&data->read_wait))
		wake_up_interruptible(&data->read_wait);

	return true;
}

static inline bool lrng_testing_have_data(struct lrng_testing *data)
{
	return ((data->rb_writer & LRNG_TESTING_RINGBUFFER_MASK) !=
		 (data->rb_reader & LRNG_TESTING_RINGBUFFER_MASK));
}

static inline int lrng_testing_reader(struct lrng_testing *data, u32 *boot,
				      u8 *outbuf, u32 outbuflen)
{
	unsigned long flags;
	int collected_data = 0;

	lrng_testing_init(data, *boot);

	while (outbuflen) {
		spin_lock_irqsave(&data->lock, flags);

		/* We have no data or reached the writer. */
		if (!data->rb_writer ||
		    (data->rb_writer == data->rb_reader)) {

			spin_unlock_irqrestore(&data->lock, flags);

			/*
			 * Now we gathered all boot data, enable regular data
			 * collection.
			 */
			if (*boot) {
				*boot = 0;
				goto out;
			}

			wait_event_interruptible(data->read_wait,
						 lrng_testing_have_data(data));
			if (signal_pending(current)) {
				collected_data = -ERESTARTSYS;
				goto out;
			}

			continue;
		}

		/* We copy out word-wise */
		if (outbuflen < sizeof(u32)) {
			spin_unlock_irqrestore(&data->lock, flags);
			goto out;
		}

		memcpy(outbuf, &data->lrng_testing_rb[data->rb_reader],
		       sizeof(u32));
		data->rb_reader++;

		spin_unlock_irqrestore(&data->lock, flags);

		outbuf += sizeof(u32);
		outbuflen -= sizeof(u32);
		collected_data += sizeof(u32);
	}

out:
	if (!lrng_testing_have_data(data))
		lrng_testing_fini(data);
	return collected_data;
}

static int lrng_testing_extract_user(struct file *file, char __user *buf,
				     size_t nbytes, loff_t *ppos,
				     int (*reader)(u8 *outbuf, u32 outbuflen))
{
	loff_t pos = *ppos;
	u8 *tmp, *tmp_aligned;
	int ret = 0, large_request = (nbytes > 256);

	if (!nbytes)
		return 0;

	/*
	 * The intention of this interface is for collecting at least
	 * 1000 samples due to the SP800-90B requirements. So, we make no
	 * effort in avoiding allocating more memory that actually needed
	 * by the user. Hence, we allocate sufficient memory to always hold
	 * that amount of data.
	 */
	tmp = kmalloc(LRNG_TESTING_RINGBUFFER_SIZE + sizeof(u32), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	tmp_aligned = PTR_ALIGN(tmp, sizeof(u32));

	while (nbytes) {
		int i;

		if (large_request && need_resched()) {
			if (signal_pending(current)) {
				if (ret == 0)
					ret = -ERESTARTSYS;
				break;
			}
			schedule();
		}

		i = min_t(int, nbytes, LRNG_TESTING_RINGBUFFER_SIZE);
		i = reader(tmp_aligned, i);
		if (i <= 0) {
			if (i < 0)
				ret = i;
			break;
		}
		if (copy_to_user(buf, tmp_aligned, i)) {
			ret = -EFAULT;
			break;
		}

		nbytes -= i;
		buf += i;
		ret += i;
	}

	kzfree(tmp);

	nbytes -= ret;
	*ppos = pos + nbytes;

	return ret;
}

/************************* Raw Entropy Data Handling **************************/

#ifdef CONFIG_LRNG_RAW_ENTROPY

static u32 boot_test = 0;
module_param(boot_test, uint, 0644);
MODULE_PARM_DESC(boot_test, "Enable gathering boot time entropy of the first entropy events");

static struct lrng_testing lrng_raw = {
	.rb_reader = 0,
	.rb_reader = 0,
	.lock      = __SPIN_LOCK_UNLOCKED(lrng_raw.lock),
	.read_wait = __WAIT_QUEUE_HEAD_INITIALIZER(lrng_raw.read_wait)
};

bool lrng_raw_entropy_store(u32 value)
{
	return lrng_testing_store(&lrng_raw, value, &boot_test);
}

static int lrng_raw_entropy_reader(u8 *outbuf, u32 outbuflen)
{
	return lrng_testing_reader(&lrng_raw, &boot_test, outbuf, outbuflen);
}

static ssize_t lrng_raw_read(struct file *file, char __user *to,
			     size_t count, loff_t *ppos)
{
	return lrng_testing_extract_user(file, to, count, ppos,
					 lrng_raw_entropy_reader);
}

static const struct file_operations lrng_raw_fops = {
	.owner = THIS_MODULE,
	.read = lrng_raw_read,
};

#endif /* CONFIG_LRNG_RAW_ENTROPY */

/********************** Raw Entropy Array Data Handling ***********************/

#ifdef CONFIG_LRNG_RAW_ARRAY

static u32 boot_raw_array = 0;
module_param(boot_raw_array, uint, 0644);
MODULE_PARM_DESC(boot_raw_array, "Enable gathering boot time raw noise array data of the first entropy events");

static struct lrng_testing lrng_raw_array = {
	.rb_reader = 0,
	.rb_reader = 0,
	.lock      = __SPIN_LOCK_UNLOCKED(lrng_raw_array.lock),
	.read_wait = __WAIT_QUEUE_HEAD_INITIALIZER(lrng_raw_array.read_wait)
};

bool lrng_raw_array_entropy_store(u32 value)
{
	return lrng_testing_store(&lrng_raw_array, value, &boot_raw_array);
}

static int lrng_raw_array_entropy_reader(u8 *outbuf, u32 outbuflen)
{
	return lrng_testing_reader(&lrng_raw_array, &boot_raw_array, outbuf,
				   outbuflen);
}

static ssize_t lrng_raw_array_read(struct file *file, char __user *to,
				   size_t count, loff_t *ppos)
{
	return lrng_testing_extract_user(file, to, count, ppos,
					 lrng_raw_array_entropy_reader);
}

static const struct file_operations lrng_raw_array_fops = {
	.owner = THIS_MODULE,
	.read = lrng_raw_array_read,
};

#endif /* CONFIG_LRNG_RAW_ARRAY */

/******************** Interrupt Performance Data Handling *********************/

#ifdef CONFIG_LRNG_IRQ_PERF

static u32 boot_irq_perf = 0;
module_param(boot_irq_perf, uint, 0644);
MODULE_PARM_DESC(boot_irq_perf, "Enable gathering boot time interrupt performance data of the first entropy events");

static struct lrng_testing lrng_irq_perf = {
	.rb_reader = 0,
	.rb_reader = 0,
	.lock      = __SPIN_LOCK_UNLOCKED(lrng_irq_perf.lock),
	.read_wait = __WAIT_QUEUE_HEAD_INITIALIZER(lrng_irq_perf.read_wait)
};

bool lrng_perf_time(u32 start)
{
	return lrng_testing_store(&lrng_irq_perf, random_get_entropy() - start,
				  &boot_irq_perf);
}

static int lrng_irq_perf_reader(u8 *outbuf, u32 outbuflen)
{
	return lrng_testing_reader(&lrng_irq_perf, &boot_irq_perf, outbuf,
				   outbuflen);
}

static ssize_t lrng_irq_perf_read(struct file *file, char __user *to,
				  size_t count, loff_t *ppos)
{
	return lrng_testing_extract_user(file, to, count, ppos,
					 lrng_irq_perf_reader);
}

static const struct file_operations lrng_irq_perf_fops = {
	.owner = THIS_MODULE,
	.read = lrng_irq_perf_read,
};

#endif /* CONFIG_LRNG_IRQ_PERF */

/**************************************************************************
 * Debugfs interface
 **************************************************************************/

static int __init lrng_raw_init(void)
{
	struct dentry *lrng_raw_debugfs_root;


	lrng_raw_debugfs_root = debugfs_create_dir(KBUILD_MODNAME, NULL);

#ifdef CONFIG_LRNG_RAW_ENTROPY
	debugfs_create_file_unsafe("lrng_raw", 0400, lrng_raw_debugfs_root,
				   NULL, &lrng_raw_fops);
#endif
#ifdef CONFIG_LRNG_RAW_ARRAY
	debugfs_create_file_unsafe("lrng_raw_array", 0400,
				   lrng_raw_debugfs_root, NULL,
				   &lrng_raw_array_fops);
#endif
#ifdef CONFIG_LRNG_IRQ_PERF
	debugfs_create_file_unsafe("lrng_irq_perf", 0400, lrng_raw_debugfs_root,
				   NULL, &lrng_irq_perf_fops);
#endif

	return 0;
}

module_init(lrng_raw_init);
