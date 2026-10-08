# SPDX-License-Identifier: GPL-2.0-or-later

ifneq ($(KERNELRELEASE),)
obj-m := gpgpu_pci.o
ccflags-y += -I$(src)/../include
else
KDIR ?= /lib/modules/$(shell uname -r)/build
MODULE_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_DIR ?= $(abspath $(MODULE_DIR)/../../../build/gpgpu-linux-module)
PYTHON ?= python3
IOMMU ?= none

.PHONY: all clean prepare module test test-fast stack-test stack-test-fast run

all:
	$(MAKE) -C "$(KDIR)" M="$(MODULE_DIR)" MO="$(BUILD_DIR)" modules

clean:
	$(MAKE) -C "$(KDIR)" M="$(MODULE_DIR)" MO="$(BUILD_DIR)" clean

prepare:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" prepare \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)"

module:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" module \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)"

test:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" test \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)"

test-fast:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" test --skip-prepare \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)"

stack-test:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" stack-test \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)"

stack-test-fast:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" stack-test --skip-prepare \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)"

run:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" run \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)"
endif
