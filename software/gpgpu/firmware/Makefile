# SPDX-License-Identifier: GPL-2.0-or-later
ROOT := ../../..
BUILD_DIR ?= $(ROOT)/build/gpgpu-firmware
TOOLCHAIN := $(abspath $(ROOT)/build/gpgpu-cp-toolchain/root/usr/bin)
CROSS_COMPILE ?= $(if $(wildcard $(TOOLCHAIN)/riscv64-unknown-elf-gcc),$(TOOLCHAIN)/riscv64-unknown-elf-,riscv64-unknown-elf-)
CC := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
CFLAGS := -march=rv64im_zicsr -mabi=lp64 -mcmodel=medany -O2 -g \
          -ffreestanding -fno-builtin -fno-pic -fno-stack-protector \
          -fno-common -msmall-data-limit=0 -mno-relax -Wall -Wextra -Werror \
          -I../include -std=c11
OBJECTS := $(addprefix $(BUILD_DIR)/,start.o main.o ring.o mgmt.o aql.o ih.o mem.o)

.PHONY: all clean
all: $(BUILD_DIR)/gpgpu-cp.bin

$(BUILD_DIR):
	mkdir -p $@
$(BUILD_DIR)/%.o: %.c cp.h $(wildcard ../include/gpgpu/*.h) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/start.o: start.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/gpgpu-cp.elf: $(OBJECTS) link.ld
	$(CC) $(CFLAGS) -nostdlib -Wl,--build-id=none,-T,link.ld,-Map,$(BUILD_DIR)/gpgpu-cp.map $(OBJECTS) -o $@
$(BUILD_DIR)/gpgpu-cp.bin: $(BUILD_DIR)/gpgpu-cp.elf
	$(OBJCOPY) -O binary $< $@
clean:
	rm -f $(OBJECTS) $(BUILD_DIR)/gpgpu-cp.elf $(BUILD_DIR)/gpgpu-cp.bin $(BUILD_DIR)/gpgpu-cp.map
