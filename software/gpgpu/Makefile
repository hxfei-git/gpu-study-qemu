# SPDX-License-Identifier: GPL-2.0-or-later
#
# 用户态部分：libgpgpu.a（KMT 薄层 + 小运行时）、三个 RV32 Kernel 与示例程序。
# 客体中构建时，Kernel 头文件由宿主机预先生成在 BUILD_DIR（客体没有 Python）。
CC ?= cc
AR ?= ar
PYTHON ?= python3
BUILD_DIR ?= ../../build/gpgpu-stack
CPPFLAGS += -Iinclude -Ilib/include -Ilib/kmt -I$(BUILD_DIR)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -pthread
LDLIBS += -pthread -lm

KERNELS = vector_add matmul relu
KERNEL_HEADERS = $(addprefix $(BUILD_DIR)/,$(addsuffix .h,$(KERNELS)))
KERNEL_BINARIES = $(addprefix $(BUILD_DIR)/,$(addsuffix .bin,$(KERNELS)))
LIB_SOURCES = $(wildcard lib/kmt/*.c lib/rt/*.c)
LIB_OBJECTS = $(patsubst lib/%.c,$(BUILD_DIR)/lib/%.o,$(LIB_SOURCES))
LIB_HEADERS = $(wildcard lib/include/*.h lib/kmt/*.h include/gpgpu/*.h include/gpgpu/uapi/*.h)

.PHONY: all kernels firmware check check-bootstrap check-configure clean
all: $(BUILD_DIR)/libgpgpu.a $(BUILD_DIR)/gpgpu-demo
kernels: $(KERNEL_HEADERS) $(KERNEL_BINARIES)

$(BUILD_DIR) $(BUILD_DIR)/lib/kmt $(BUILD_DIR)/lib/rt:
	mkdir -p $@

# 汇编器一次生成 .bin 与 .h；.bin 依赖 .h，二者由同一条命令产生。
$(BUILD_DIR)/%.h: kernels/%.S kernels/gpgpu_kernel.inc kernels/assemble.py | $(BUILD_DIR)
	$(PYTHON) kernels/assemble.py $< --bin $(BUILD_DIR)/$*.bin --header $@ --symbol $*_code
$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.h ;

$(BUILD_DIR)/lib/%.o: lib/%.c $(LIB_HEADERS) | $(BUILD_DIR)/lib/kmt $(BUILD_DIR)/lib/rt
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/libgpgpu.a: $(LIB_OBJECTS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/gpgpu-demo: demos/gpgpu-demo.c $(KERNEL_HEADERS) $(BUILD_DIR)/libgpgpu.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD_DIR)/libgpgpu.a $(LDLIBS) -o $@

firmware:
	$(MAKE) -C firmware

# 汇编器编码回归，不需要设备。
check:
	$(PYTHON) kernels/test_assemble.py

# 基础环境的缓存与磁盘保护回归，不需要设备或网络。
check-bootstrap:
	$(PYTHON) scripts/test_bootstrap_env.py

# QEMU 配置入口回归，使用模拟子进程，不需要客体内核或网络。
check-configure:
	$(PYTHON) scripts/test_gpgpu_linux.py

clean:
	rm -rf $(BUILD_DIR)/lib $(BUILD_DIR)/libgpgpu.a $(BUILD_DIR)/gpgpu-demo
	rm -f $(KERNEL_HEADERS) $(KERNEL_BINARIES)
