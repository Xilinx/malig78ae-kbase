# SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
#
# (C) COPYRIGHT 2026 AMD/Xilinx. All rights reserved.
#
# Mali GPU PM Module Makefile
#

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

obj-m := mali_gpu_pm.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
