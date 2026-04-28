/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Mali GPU PM - Header
 *
 * (C) COPYRIGHT 2026 AMD/Xilinx. All rights reserved.
 *
 * This driver accesses SLCR registers using zynqmp_pm_sec_* APIs:
 */

#ifndef _MALI_GPU_PM_H_
#define _MALI_GPU_PM_H_

/*
 * For zynqmp_pm_sec_* calls, offset from pm_pmc_regnode base (if using
 * a page-aligned PM node for the 0xEB450000 region).
 */
#define PSXC_LPX_SLCR_EAM_PMC3_DIS_OFFSET	0x401AC

/* MMI_GPU_SLCR interrupt status (sticky) registers — clear via PM API */
#define MMI_GPU_SLCR_CORR_IR_STATUS_OFFSET	0x200
#define MMI_GPU_SLCR_UNCORR_IR_STATUS_OFFSET	0x414
#define MMI_GPU_SLCR_MISC_IR_STATUS_OFFSET	0x428

/* Register Values */
#define GPU_PLM_PMC_MASK_VALUE		0x400U	/* Value to mask PMC errors */
#define GPU_PLM_PMC_UNMASK_VALUE	0x0U	/* Value to unmask PMC errors */

/*
 * PM API Regnode IDs (Firmware-specific node identifiers)
 * These are used with zynqmp_pm_sec_mask_write_reg() and zynqmp_pm_sec_read_reg()
 */
#define PM_REG_LPD_SLCR				(0x30000007U)
#define PM_REG_MMI_GPU_SLCR			(0x30000008U)

#endif /* _MALI_GPU_PM_H_ */
