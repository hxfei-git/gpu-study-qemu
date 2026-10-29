# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= cc
AR ?= ar
PYTHON ?= python3
BUILD_DIR ?= ../../build/gpgpu-stack
CPPFLAGS += -Iinclude -Ihsakmt -I$(BUILD_DIR)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -pthread
LDLIBS += -pthread -lm
KERNELS = vector_add matmul relu
KERNEL_HEADERS = $(addprefix $(BUILD_DIR)/,$(addsuffix .h,$(KERNELS)))
KERNEL_BINARIES = $(addprefix $(BUILD_DIR)/,$(addsuffix .bin,$(KERNELS)))
KMT_OBJECTS = $(addprefix $(BUILD_DIR)/kmt-,device.o memory.o queue.o event.o submit.o)

.PHONY: all kernels firmware clean
all: $(BUILD_DIR)/libgpgpu-kmt.a $(BUILD_DIR)/gpgpu-demo
kernels: $(KERNEL_HEADERS) $(KERNEL_BINARIES)
$(BUILD_DIR):
	mkdir -p $@
$(BUILD_DIR)/%.h $(BUILD_DIR)/%.bin &: kernels/%.S kernels/gpgpu_kernel.inc kernels/assemble.py | $(BUILD_DIR)
	$(PYTHON) kernels/assemble.py $< --bin $(BUILD_DIR)/$*.bin --header $(BUILD_DIR)/$*.h --symbol $*_code
$(BUILD_DIR)/kmt-%.o: hsakmt/%.c hsakmt/gpgpu_kmt.h hsakmt/internal.h $(wildcard include/gpgpu/*.h include/gpgpu/uapi/*.h) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/libgpgpu-kmt.a: $(KMT_OBJECTS)
	$(AR) rcs $@ $^
$(BUILD_DIR)/gpgpu-demo: demos/gpgpu-demo.c $(KERNEL_HEADERS) $(BUILD_DIR)/libgpgpu-kmt.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD_DIR)/libgpgpu-kmt.a $(LDLIBS) -o $@
firmware:
	$(MAKE) -C firmware
clean:
	rm -f $(KMT_OBJECTS) $(BUILD_DIR)/libgpgpu-kmt.a $(BUILD_DIR)/gpgpu-demo
	rm -f $(KERNEL_HEADERS) $(KERNEL_BINARIES)
