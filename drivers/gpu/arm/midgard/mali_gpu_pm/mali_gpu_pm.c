// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
/*
 * Mali GPU PM - Register access for GPU initialization using PM API
 *
 * (C) COPYRIGHT 2026 AMD/Xilinx. All rights reserved.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/firmware/xlnx-zynqmp.h>

#include "mali_gpu_pm.h"

#define DRV_NAME	"mali_gpu_pm"
#define KBASE_NAME	"mali"
/*
 * The SLCR status bits cannot be cleared while a PTM message is unread, so
 * retry with a doubling sleep until SLCR_CLEAR_TIMEOUT_US; may sleep.
 */
#define SLCR_CLEAR_TIMEOUT_US	250000
#define SLCR_RETRY_DELAY_US	100U
#define SLCR_RETRY_MAX_DELAY_US	10000U
#define SLCR_SETTLE_RECHECK_US	100

/* State variables */
static DEFINE_MUTEX(lock);
static bool pmc_masked;
static bool finish_done;
static bool post_init_pending;
static bool exiting;

/* Notifier */
static struct notifier_block bus_nb;
static bool nb_registered;

/* MMI_GPU_SLCR register offsets */
static const u32 slcr_offsets[] = {
	MMI_GPU_SLCR_CORR_IR_STATUS_OFFSET,
	MMI_GPU_SLCR_UNCORR_IR_STATUS_OFFSET,
	MMI_GPU_SLCR_MISC_IR_STATUS_OFFSET,
};

static int write_pmc_register(u32 value)
{
	int ret = 0;

	ret = zynqmp_pm_sec_mask_write_reg(PM_REG_LPD_SLCR,
					   PSXC_LPX_SLCR_EAM_PMC3_DIS_OFFSET,
					   GENMASK(10, 10), value);
	if (ret) {
		pr_err("%s: zynqmp_pm_sec_mask_write_reg failed: %d\n", DRV_NAME, ret);
		return ret;
	}

	return 0;
}

static int mask_pmc_register(void)
{
	int ret = 0;

	if (pmc_masked)
		return 0;

	ret = write_pmc_register(GPU_PLM_PMC_MASK_VALUE);
	if (ret) {
		pr_err("%s: PMC mask failed: %d\n", DRV_NAME, ret);
		return ret;
	}

	pmc_masked = true;
	return 0;
}

static int unmask_pmc_register(void)
{
	int ret = 0;

	if (!pmc_masked)
		return 0;

	ret = write_pmc_register(GPU_PLM_PMC_UNMASK_VALUE);
	if (ret) {
		pr_err("%s: PMC unmask failed: %d\n", DRV_NAME, ret);
		return ret;
	}

	pmc_masked = false;
	return 0;
}

static int clear_single_slcr_register(u32 offset, ktime_t deadline)
{
	unsigned int delay_us = SLCR_RETRY_DELAY_US;
	unsigned int tries = 0;
	u32 value = 0, verify = 0;
	int ret = 0;

	do {
		if (tries) {
			fsleep(delay_us);
			delay_us = min(delay_us * 2, SLCR_RETRY_MAX_DELAY_US);
		}

		ret = zynqmp_pm_sec_read_reg(PM_REG_MMI_GPU_SLCR, offset, &value);
		if (ret) {
			pr_err("%s: zynqmp_pm_sec_read_reg(0x%x) failed: %d\n",
			       DRV_NAME, offset, ret);
			return ret;
		}

		if (!value)
			return 0;

		tries++;
		ret = zynqmp_pm_sec_mask_write_reg(PM_REG_MMI_GPU_SLCR, offset,
						   GENMASK(31, 0), value);
		if (ret) {
			pr_err("%s: zynqmp_pm_sec_mask_write_reg(0x%x) failed: %d\n",
			       DRV_NAME, offset, ret);
			return ret;
		}

		ret = zynqmp_pm_sec_read_reg(PM_REG_MMI_GPU_SLCR, offset, &verify);
		if (ret) {
			pr_err("%s: verify read(0x%x) failed: %d\n",
			       DRV_NAME, offset, ret);
			return ret;
		}

		if (!verify) {
			/* Ensure clear is stable and not re-asserted immediately. */
			udelay(SLCR_SETTLE_RECHECK_US);
			ret = zynqmp_pm_sec_read_reg(PM_REG_MMI_GPU_SLCR, offset, &verify);
			if (ret) {
				pr_err("%s: settle verify read(0x%x) failed: %d\n",
				       DRV_NAME, offset, ret);
				return ret;
			}

			if (!verify)
				return 0;
		}
	} while (ktime_before(ktime_get(), deadline));

	pr_err("%s: SLCR+0x%x failed to clear (0x%x) after %u tries\n",
	       DRV_NAME, offset, verify, tries);
	return -EIO;
}

static int clear_slcr_registers(void)
{
	ktime_t deadline = ktime_add_us(ktime_get(), SLCR_CLEAR_TIMEOUT_US);
	unsigned int i = 0;
	int ret = 0, err = 0;

	pr_debug("%s: clear_slcr_registers() - start\n", DRV_NAME);

	for (i = 0; i < ARRAY_SIZE(slcr_offsets); i++) {
		ret = clear_single_slcr_register(slcr_offsets[i], deadline);
		if (ret && !err)
			err = ret;
	}

	return err;
}

static int complete_post_init_sequence(void)
{
	int ret = 0, err = 0;

	ret = clear_slcr_registers();
	if (ret) {
		pr_warn("%s: SLCR clear had errors: %d\n", DRV_NAME, ret);
		err = ret;
	}

	ret = unmask_pmc_register();
	if (ret) {
		pr_warn("%s: PMC unmask failed: %d\n", DRV_NAME, ret);
		if (!err)
			err = ret;
	}

	finish_done = true;
	post_init_pending = false;
	pr_info("%s: Post-init sequence Complete\n", DRV_NAME);
	return err;
}

static bool is_kbase_gpu(const struct device_node *np)
{
	return np && of_device_is_compatible(np, "arm,mali-midgard");
}

static int match_mali_gpu(struct device *dev, const void *unused)
{
	(void)unused;
	return is_kbase_gpu(dev->of_node);
}

static bool is_kbase_driver(struct device *dev)
{
	if (!dev || !dev->driver || !dev->driver->name)
		return false;
	if (strcmp(dev->driver->name, KBASE_NAME) != 0)
		return false;
	return is_kbase_gpu(dev->of_node);
}

static int bus_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct device *dev = data;
	int ret = 0;
	(void)nb;

	if (action != BUS_NOTIFY_BOUND_DRIVER || !is_kbase_driver(dev))
		return NOTIFY_OK;

	pr_info("%s: bus_notify: kbase driver bound detected!\n", DRV_NAME);

	mutex_lock(&lock);

	if (exiting || finish_done || !post_init_pending) {
		pr_info("%s: Sequence already completed, skipping\n", DRV_NAME);
		mutex_unlock(&lock);
		return NOTIFY_OK;
	}

	ret = complete_post_init_sequence();
	if (ret)
		pr_warn("%s: post-init sequence completed with errors: %d\n",
			DRV_NAME, ret);

	mutex_unlock(&lock);

	return NOTIFY_OK;
}

static int __init mali_gpu_pm_init(void)
{
	int ret = 0, unmask_ret = 0;
	struct device *gpu_dev = NULL;

	pr_info("%s: mali_gpu_pm_init()\n", DRV_NAME);
	exiting = false;
	post_init_pending = false;

	/*
	 * Check if kbase is already bound - if so, skip masking sequence.
	 * This handles the case of consecutive module reloads where the
	 * GPU is already initialized.
	 */
	gpu_dev = bus_find_device(&platform_bus_type, NULL, NULL, match_mali_gpu);
	if (gpu_dev && is_kbase_driver(gpu_dev)) {
		pr_debug("%s: kbase already bound, skipping PMC masking sequence\n",
			DRV_NAME);
		put_device(gpu_dev);

		mutex_lock(&lock);
		finish_done = true;
		post_init_pending = false;
		mutex_unlock(&lock);

		return 0;
	}
	if (gpu_dev)
		put_device(gpu_dev);

	mutex_lock(&lock);
	ret = mask_pmc_register();
	if (ret) {
		mutex_unlock(&lock);
		pr_err("%s: Failed to mask PMC error: %d\n", DRV_NAME, ret);
		return ret;
	}
	post_init_pending = true;

	bus_nb.notifier_call = bus_notify;
	ret = bus_register_notifier(&platform_bus_type, &bus_nb);
	if (ret) {
		pr_err("%s: bus_register_notifier failed: %d\n", DRV_NAME, ret);
		post_init_pending = false;
		unmask_ret = unmask_pmc_register();
		if (unmask_ret) {
			pr_err("%s: cleanup unmask failed after notifier error: %d\n",
			       DRV_NAME, unmask_ret);
		}
		mutex_unlock(&lock);
		return ret;
	}
	nb_registered = true;

	/*
	 * Re-check after notifier registration: kbase may have bound in the
	 * window between the initial probe and notifier install.
	 */
	gpu_dev = bus_find_device(&platform_bus_type, NULL, NULL, match_mali_gpu);
	if (gpu_dev && is_kbase_driver(gpu_dev) && !finish_done &&
	    post_init_pending && !exiting) {
		ret = complete_post_init_sequence();
		if (ret)
			pr_warn("%s: post-init sequence completed with errors: %d\n",
				DRV_NAME, ret);
	}
	if (gpu_dev)
		put_device(gpu_dev);

	mutex_unlock(&lock);

	return 0;
}

static void __exit mali_gpu_pm_exit(void)
{
	bool do_unregister = false;
	int unmask_ret = 0;

	pr_info("%s: mali_gpu_pm_exit()\n", DRV_NAME);

	/* Set exit state first so concurrent notifier callbacks
	 * return immediately without running post-init work.
	 */
	mutex_lock(&lock);
	exiting = true;
	post_init_pending = false;
	do_unregister = nb_registered;
	nb_registered = false;
	mutex_unlock(&lock);

	if (do_unregister)
		bus_unregister_notifier(&platform_bus_type, &bus_nb);

	mutex_lock(&lock);
	if (pmc_masked) {
		unmask_ret = unmask_pmc_register();
		if (unmask_ret) {
			pr_err("%s: failed to unmask PMC during exit: %d\n",
			       DRV_NAME, unmask_ret);
		}
	}
	mutex_unlock(&lock);
}

module_init(mali_gpu_pm_init);
module_exit(mali_gpu_pm_exit);

MODULE_DESCRIPTION("Mali GPU PM - Register access for GPU initialization");
MODULE_AUTHOR("AMD/Xilinx");
MODULE_LICENSE("GPL");
