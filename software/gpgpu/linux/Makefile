# SPDX-License-Identifier: GPL-2.0-or-later

ifneq ($(KERNELRELEASE),)
obj-m := gpgpu_pci.o
gpgpu_pci-y := gpgpu_pci_main.o gpgpu_memory.o gpgpu_queue.o gpgpu_event.o gpgpu_gpuvm.o
ccflags-y += -I$(src)/../include
else
KDIR ?= /lib/modules/$(shell uname -r)/build
MODULE_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
SCRIPTS_DIR := $(abspath $(MODULE_DIR)/../scripts)
BUILD_DIR ?= $(abspath $(MODULE_DIR)/../../../build/gpgpu-kernel-gpuvm)
PYTHON ?= python3
IOMMU ?= none
GPUVM ?= on
DEVICE_QEMU ?= $(MODULE_DIR)/../../../build/arm64-qemu/qemu-system-riscv64
.PHONY: all clean kernel prepare module run

all:
	$(MAKE) -C "$(KDIR)" M="$(MODULE_DIR)" MO="$(BUILD_DIR)" modules

clean:
	$(MAKE) -C "$(KDIR)" M="$(MODULE_DIR)" MO="$(BUILD_DIR)" clean

kernel:
	$(PYTHON) "$(SCRIPTS_DIR)/kernel-build.py"

prepare module:
	$(PYTHON) "$(SCRIPTS_DIR)/gpgpu_linux.py" $@ --output-dir "$(BUILD_DIR)"

run:
	$(PYTHON) "$(SCRIPTS_DIR)/gpgpu_linux.py" run --output-dir "$(BUILD_DIR)" \
		--iommu "$(IOMMU)" --gpuvm "$(GPUVM)" --device-qemu "$(DEVICE_QEMU)"
endif
