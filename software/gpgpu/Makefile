# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= cc
AR ?= ar
PYTHON ?= python3
BUILD_DIR ?= ../../build/gpgpu-stack
CPPFLAGS += -Iinclude -Iruntime -Ihsakmt -I$(BUILD_DIR)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -pthread
LDLIBS += -pthread -lm
KERNELS = vector_add matmul relu builtin_index
KERNEL_HEADERS = $(addprefix $(BUILD_DIR)/,$(addsuffix .h,$(KERNELS)))
KERNEL_BINARIES = $(addprefix $(BUILD_DIR)/,$(addsuffix .bin,$(KERNELS)))

.PHONY: all kernels check-assembler firmware clean
all: $(BUILD_DIR)/libgpgpu.a $(BUILD_DIR)/gpgpu-demo $(BUILD_DIR)/test-runtime \
     $(BUILD_DIR)/libgpgpu-kmt.a $(BUILD_DIR)/test-queue

kernels: $(KERNEL_HEADERS) $(KERNEL_BINARIES)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.h $(BUILD_DIR)/%.bin: kernels/%.S kernels/gpgpu_kernel.inc \
                                    kernels/assemble.py | $(BUILD_DIR)
	$(PYTHON) kernels/assemble.py $< --bin $(BUILD_DIR)/$*.bin \
		--header $(BUILD_DIR)/$*.h --symbol $*_code

$(BUILD_DIR)/gpgpu.o: runtime/gpgpu.c runtime/gpgpu.h include/gpgpu_uapi.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/libgpgpu.a: $(BUILD_DIR)/gpgpu.o
	$(AR) rcs $@ $<

$(BUILD_DIR)/gpgpu-demo: demos/gpgpu-demo.c $(KERNEL_HEADERS) \
                        $(BUILD_DIR)/libgpgpu.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD_DIR)/libgpgpu.a $(LDLIBS) -o $@

$(BUILD_DIR)/test-runtime: tests/test-runtime.c $(BUILD_DIR)/builtin_index.h \
                          $(BUILD_DIR)/libgpgpu.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD_DIR)/libgpgpu.a $(LDLIBS) -o $@

check-assembler:
	$(PYTHON) tests/test-assembler.py

clean:
	rm -f $(BUILD_DIR)/gpgpu.o $(BUILD_DIR)/libgpgpu.a
	rm -f $(BUILD_DIR)/gpgpu-demo $(BUILD_DIR)/test-runtime
	rm -f $(KERNEL_HEADERS) $(KERNEL_BINARIES)
	rm -f $(KMT_OBJECTS) $(BUILD_DIR)/libgpgpu-kmt.a $(BUILD_DIR)/test-queue

KMT_OBJECTS = $(addprefix $(BUILD_DIR)/kmt-,device.o memory.o queue.o event.o)
$(BUILD_DIR)/kmt-%.o: hsakmt/%.c hsakmt/gpgpu_kmt.h hsakmt/internal.h include/gpgpu_queue.h include/gpgpu_uapi.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/libgpgpu-kmt.a: $(KMT_OBJECTS)
	$(AR) rcs $@ $^
$(BUILD_DIR)/test-queue: tests/test-queue.c tests/queue_helpers.c tests/queue_helpers.h $(KERNEL_HEADERS) $(BUILD_DIR)/libgpgpu-kmt.a
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test-queue.c tests/queue_helpers.c $(BUILD_DIR)/libgpgpu-kmt.a $(LDLIBS) -o $@
firmware:
	$(MAKE) -C firmware
