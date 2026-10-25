# SPDX-License-Identifier: GPL-2.0-or-later

ifneq ($(KERNELRELEASE),)
obj-m := gpgpu_pci.o
gpgpu_pci-y := gpgpu_pci_main.o gpgpu_memory.o gpgpu_queue.o gpgpu_event.o gpgpu_gpuvm.o
ccflags-y += -I$(src)/../include
else
KDIR ?= /lib/modules/$(shell uname -r)/build
MODULE_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
SCRIPTS_DIR := $(abspath $(MODULE_DIR)/../scripts)
BUILD_DIR ?= $(abspath $(MODULE_DIR)/../../../build/gpgpu-linux-module)
GPUVM_BUILD_DIR ?= $(abspath $(MODULE_DIR)/../../../build/gpgpu-kernel-gpuvm)
PYTHON ?= python3
IOMMU ?= none
REMOTE ?= 0
DEVICE_QEMU ?= $(MODULE_DIR)/../../../build/arm64-qemu/qemu-system-riscv64
REMOTE_FLAGS = $(if $(filter 1,$(REMOTE)),--remote --device-qemu "$(DEVICE_QEMU)")

.PHONY: all clean kernel prepare module test test-fast stack-test stack-test-fast queue-test run
.PHONY: test-gpuvm test-gpuvm-fault

all:
	$(MAKE) -C "$(KDIR)" M="$(MODULE_DIR)" MO="$(BUILD_DIR)" modules

clean:
	$(MAKE) -C "$(KDIR)" M="$(MODULE_DIR)" MO="$(BUILD_DIR)" clean

kernel:
	$(PYTHON) "$(SCRIPTS_DIR)/kernel-build.py"

prepare:
	$(PYTHON) "$(SCRIPTS_DIR)/gpgpu_linux.py" prepare \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)"

module:
	$(PYTHON) "$(SCRIPTS_DIR)/gpgpu_linux.py" module \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)"

test:
	$(PYTHON) "$(SCRIPTS_DIR)/gpgpu_linux.py" test \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)" $(REMOTE_FLAGS)

test-fast:
	$(PYTHON) "$(SCRIPTS_DIR)/gpgpu_linux.py" test --skip-prepare \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)" $(REMOTE_FLAGS)

stack-test:
	$(PYTHON) "$(SCRIPTS_DIR)/gpgpu_linux.py" stack-test \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)" $(REMOTE_FLAGS)

stack-test-fast:
	$(PYTHON) "$(SCRIPTS_DIR)/gpgpu_linux.py" stack-test --skip-prepare \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)" $(REMOTE_FLAGS)

queue-test:
	$(PYTHON) "$(SCRIPTS_DIR)/gpgpu_linux.py" queue-test \
		--output-dir "$(BUILD_DIR)" --iommu none --remote --device-qemu "$(DEVICE_QEMU)"

test-gpuvm:
	$(PYTHON) "$(SCRIPTS_DIR)/gpgpu_linux.py" gpuvm-test \
		--output-dir "$(GPUVM_BUILD_DIR)" --iommu "$(IOMMU)" --device-qemu "$(DEVICE_QEMU)"

test-gpuvm-fault:
	$(PYTHON) "$(SCRIPTS_DIR)/gpgpu_linux.py" gpuvm-fault-test \
		--output-dir "$(GPUVM_BUILD_DIR)" --device-qemu "$(DEVICE_QEMU)"

run:
	$(PYTHON) "$(SCRIPTS_DIR)/gpgpu_linux.py" run \
		--output-dir "$(BUILD_DIR)" --iommu "$(IOMMU)" $(REMOTE_FLAGS)
endif
