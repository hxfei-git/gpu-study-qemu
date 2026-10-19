# SPDX-License-Identifier: GPL-2.0-or-later

ifneq ($(KERNELRELEASE),)
obj-m := gpgpu_pci.o
gpgpu_pci-y := gpgpu_pci_main.o gpgpu_memory.o gpgpu_queue.o gpgpu_event.o
ccflags-y += -I$(src)/../include
else
KDIR ?= /lib/modules/$(shell uname -r)/build
MODULE_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_DIR ?= $(abspath $(MODULE_DIR)/../../../build/gpgpu-linux-module)
PYTHON ?= python3
IOMMU ?= none
REMOTE ?= 0
DEVICE_QEMU ?= $(MODULE_DIR)/../../../build/arm64-qemu/qemu-system-riscv64
REMOTE_FLAGS = $(if $(filter 1,$(REMOTE)),--remote --device-qemu "$(DEVICE_QEMU)")

.PHONY: all clean kernel prepare module test test-fast stack-test stack-test-fast queue-test run

all:
	$(MAKE) -C "$(KDIR)" M="$(MODULE_DIR)" MO="$(BUILD_DIR)" modules

clean:
	$(MAKE) -C "$(KDIR)" M="$(MODULE_DIR)" MO="$(BUILD_DIR)" clean

kernel:
	$(PYTHON) "$(MODULE_DIR)/kernel-build.py"

prepare:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" prepare \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)"

module:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" module \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)"

test:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" test \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)" $(REMOTE_FLAGS)

test-fast:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" test --skip-prepare \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)" $(REMOTE_FLAGS)

stack-test:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" stack-test \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)" $(REMOTE_FLAGS)

stack-test-fast:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" stack-test --skip-prepare \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)" $(REMOTE_FLAGS)

queue-test:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" queue-test \
		--output-dir "$(BUILD_DIR)" --iommu none --remote --device-qemu "$(DEVICE_QEMU)"

run:
	$(PYTHON) "$(MODULE_DIR)/module-test.py" run \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)" $(REMOTE_FLAGS)
endif
