# SPDX-License-Identifier: GPL-2.0-or-later

ifneq ($(KERNELRELEASE),)
obj-m := gpgpu_pci.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build
MODULE_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_DIR ?= $(abspath $(MODULE_DIR)/../../../build/gpgpu-linux-module)
PYTHON ?= python3

.PHONY: all clean prepare test run

all:
	$(MAKE) -C "$(KDIR)" M="$(MODULE_DIR)" MO="$(BUILD_DIR)" modules

clean:
	$(MAKE) -C "$(KDIR)" M="$(MODULE_DIR)" MO="$(BUILD_DIR)" clean

prepare:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" prepare --output-dir "$(BUILD_DIR)"

test:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" test --output-dir "$(BUILD_DIR)"

run:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" run --output-dir "$(BUILD_DIR)"
endif
