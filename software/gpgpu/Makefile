# SPDX-License-Identifier: GPL-2.0-or-later
#
# 用户态部分：libgpgpu.a（KMT 薄层 + 小运行时）、RV64 Kernel 与回放程序。
# 客体中构建时，Kernel 头文件由宿主机预先生成在 BUILD_DIR（客体没有 Python）。
CC ?= cc
AR ?= ar
HOSTCC ?= gcc
PYTHON ?= python3
BUILD_DIR ?= ../../build/gpgpu-stack
CPPFLAGS += -Iinclude -Ilib/include -Ilib/kmt -I$(BUILD_DIR)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -pthread
LDLIBS += -pthread -lm

KERNELS = vector_add matmul relu model_eval
KERNEL_HEADERS = $(addprefix $(BUILD_DIR)/,$(addsuffix .h,$(KERNELS)))
KERNEL_BINARIES = $(addprefix $(BUILD_DIR)/,$(addsuffix .bin,$(KERNELS)))
LIB_SOURCES = $(wildcard lib/kmt/*.c lib/rt/*.c)
LIB_OBJECTS = $(patsubst lib/%.c,$(BUILD_DIR)/lib/%.o,$(LIB_SOURCES))
LIB_HEADERS = $(wildcard lib/include/*.h lib/kmt/*.h include/gpgpu/*.h include/gpgpu/uapi/*.h)

.PHONY: all kernels firmware check check-bootstrap clean
all: $(BUILD_DIR)/libgpgpu.a $(BUILD_DIR)/gpgpu-demo $(BUILD_DIR)/gpgpu-model-eval
kernels: $(KERNEL_HEADERS) $(KERNEL_BINARIES)

$(BUILD_DIR) $(BUILD_DIR)/lib/kmt $(BUILD_DIR)/lib/rt:
	mkdir -p $@

# 汇编器一次生成 .bin 与 .h；.bin 依赖 .h，二者由同一条命令产生。
$(BUILD_DIR)/%.h: kernels/%.S kernels/gpgpu_kernel.inc kernels/assemble.py | $(BUILD_DIR)
	$(PYTHON) kernels/assemble.py $< --bin $(BUILD_DIR)/$*.bin --header $@ --symbol $*_code
$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.h ;

# .s 是宿主机生成的中间产物；客体只有已生成 .h 时不要求重新运行 Python。
$(BUILD_DIR)/model_eval.h: kernels/model_eval.S kernels/model_math.inc \
			include/gpgpu/gpgpu_eval_abi.h include/gpgpu/gpgpu_tensor_abi.h \
			kernels/assemble.py | $(BUILD_DIR)
	$(HOSTCC) -E -P -x assembler-with-cpp -Ikernels -Iinclude $< -o $(BUILD_DIR)/model_eval.s
	$(PYTHON) kernels/assemble.py $(BUILD_DIR)/model_eval.s --bin $(BUILD_DIR)/model_eval.bin --header $@ --symbol model_eval_code

$(BUILD_DIR)/lib/%.o: lib/%.c $(LIB_HEADERS) | $(BUILD_DIR)/lib/kmt $(BUILD_DIR)/lib/rt
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/libgpgpu.a: $(LIB_OBJECTS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/gpgpu-demo: demos/gpgpu-demo.c $(KERNEL_HEADERS) $(BUILD_DIR)/libgpgpu.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD_DIR)/libgpgpu.a $(LDLIBS) -o $@

$(BUILD_DIR)/gpgpu-model-eval: demos/gpgpu-model-eval.c $(BUILD_DIR)/model_eval.h \
				include/gpgpu/gpgpu_eval_abi.h $(BUILD_DIR)/libgpgpu.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD_DIR)/libgpgpu.a $(LDLIBS) -Wl,--wrap=ioctl -o $@

firmware:
	$(MAKE) -C firmware

# 汇编器编码回归，不需要设备。
check:
	$(PYTHON) kernels/test_assemble.py

# 基础环境的缓存与磁盘保护回归，不需要设备或网络。
check-bootstrap:
	$(PYTHON) scripts/test_bootstrap_env.py

clean:
	rm -rf $(BUILD_DIR)/lib $(BUILD_DIR)/libgpgpu.a $(BUILD_DIR)/gpgpu-demo $(BUILD_DIR)/gpgpu-model-eval
	rm -f $(KERNEL_HEADERS) $(KERNEL_BINARIES) $(BUILD_DIR)/model_eval.s
