# SPDX-License-Identifier: GPL-2.0-only
ifneq ($(KERNELRELEASE),)
obj-m := pcws.o
pcws-y := pcws_main.o pcws_cashdrawer.o pcws_board.o pcws_serial.o pcws_controls.o
ccflags-y += -I$(src)/include
else
KERNEL_VERSION ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KERNEL_VERSION)/build
.PHONY: all modules clean check-kdir
all: modules
check-kdir:
	@test -f "$(KDIR)/Makefile" -a -f "$(KDIR)/include/config/auto.conf" -a -f "$(KDIR)/Module.symvers" || { echo "Missing prepared kernel build tree: $(KDIR)"; exit 1; }
modules: check-kdir
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" modules
clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean
endif
