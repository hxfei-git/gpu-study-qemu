# SPDX-License-Identifier: GPL-2.0-or-later
#
# 用户态部分：KMT/RT、RV64 Kernel、模型运行核心与客体应用入口。
# Kernel 可由构建端预先生成；客体 Python 只负责应用输入、构图和后处理。
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
all: $(BUILD_DIR)/libgpgpu.a $(BUILD_DIR)/gpgpu-demo $(BUILD_DIR)/gpgpu-model-eval \
	$(BUILD_DIR)/gpgpu-model-run $(BUILD_DIR)/libgpgpu-model.so \
	$(BUILD_DIR)/gpgpu-qwen-chat $(BUILD_DIR)/gpgpu-yolo
kernels: $(KERNEL_HEADERS) $(KERNEL_BINARIES)

$(BUILD_DIR) $(BUILD_DIR)/lib/kmt $(BUILD_DIR)/lib/rt:
	mkdir -p $@

# 两个产物由同一命令生成；任一个缺失都必须重新汇编。
$(BUILD_DIR)/%.h $(BUILD_DIR)/%.bin &: kernels/%.S kernels/gpgpu_kernel.inc kernels/assemble.py | $(BUILD_DIR)
	$(PYTHON) kernels/assemble.py $< --bin $(BUILD_DIR)/$*.bin --header $(BUILD_DIR)/$*.h --symbol $*_code

# .s 为预处理产物；已生成的两个文件都有效时，无需重新调用构建端工具。
$(BUILD_DIR)/model_eval.h $(BUILD_DIR)/model_eval.bin &: kernels/model_eval.S kernels/model_math.inc \
			include/gpgpu/gpgpu_eval_abi.h include/gpgpu/gpgpu_tensor_abi.h \
			kernels/assemble.py | $(BUILD_DIR)
	$(HOSTCC) -E -P -x assembler-with-cpp -Ikernels -Iinclude $< -o $(BUILD_DIR)/model_eval.s
	$(PYTHON) kernels/assemble.py $(BUILD_DIR)/model_eval.s --bin $(BUILD_DIR)/model_eval.bin --header $(BUILD_DIR)/model_eval.h --symbol model_eval_code

$(BUILD_DIR)/lib/%.o: lib/%.c $(LIB_HEADERS) | $(BUILD_DIR)/lib/kmt $(BUILD_DIR)/lib/rt
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/libgpgpu.a: $(LIB_OBJECTS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/gpgpu-demo: demos/gpgpu-demo.c $(KERNEL_HEADERS) $(BUILD_DIR)/libgpgpu.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD_DIR)/libgpgpu.a $(LDLIBS) -o $@

$(BUILD_DIR)/gpgpu-model-eval: demos/gpgpu-model-eval.c demos/model_ops.c demos/model_ops.h \
				$(BUILD_DIR)/model_eval.h include/gpgpu/gpgpu_eval_abi.h \
				$(BUILD_DIR)/libgpgpu.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< demos/model_ops.c $(BUILD_DIR)/libgpgpu.a $(LDLIBS) -Wl,--wrap=ioctl -o $@

$(BUILD_DIR)/gpgpu-model-run: demos/gpgpu-model-run.c demos/model_runtime.c demos/model_runtime.h \
				demos/model_ops.c demos/model_ops.h demos/model_program.h \
				$(BUILD_DIR)/model_eval.h $(BUILD_DIR)/libgpgpu.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< demos/model_runtime.c demos/model_ops.c \
		$(BUILD_DIR)/libgpgpu.a $(LDLIBS) -o $@

# 仅应用共享库使用 PIC，不改变已有静态用户库的构建方式。
$(BUILD_DIR)/libgpgpu-model.so: demos/model_runtime.c demos/model_runtime.h demos/model_ops.c \
				demos/model_ops.h demos/model_program.h $(BUILD_DIR)/model_eval.h \
				$(LIB_SOURCES) $(LIB_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared demos/model_runtime.c demos/model_ops.c \
		$(LIB_SOURCES) $(LDLIBS) -o $@

# 路径取自本次构建位置，客体内运行时不调用 WSL 的 Python。
$(BUILD_DIR)/gpgpu-qwen-chat: scripts/gpgpu_model_apps.py scripts/guest_model_tokenizer.py \
				$(BUILD_DIR)/libgpgpu-model.so
	printf '%s\n' '#!/bin/sh' 'exec python3 -I "$(abspath scripts)/gpgpu_model_apps.py" qwen-chat --runtime "$(abspath $(BUILD_DIR))/libgpgpu-model.so" "$$@"' > $@
	chmod +x $@

$(BUILD_DIR)/gpgpu-yolo: scripts/gpgpu_model_apps.py scripts/guest_model_yolo.py \
				$(BUILD_DIR)/libgpgpu-model.so
	printf '%s\n' '#!/bin/sh' 'exec python3 -I "$(abspath scripts)/gpgpu_model_apps.py" yolo --runtime "$(abspath $(BUILD_DIR))/libgpgpu-model.so" "$$@"' > $@
	chmod +x $@

firmware:
	$(MAKE) -C firmware

# 汇编器编码回归，不需要设备。
check:
	$(PYTHON) kernels/test_assemble.py

# 基础环境的缓存与磁盘保护回归，不需要设备或网络。
check-bootstrap:
	$(PYTHON) scripts/test_bootstrap_env.py

clean:
	rm -rf $(BUILD_DIR)/lib $(BUILD_DIR)/libgpgpu.a $(BUILD_DIR)/gpgpu-demo $(BUILD_DIR)/gpgpu-model-eval $(BUILD_DIR)/gpgpu-model-run
	rm -f $(BUILD_DIR)/libgpgpu-model.so $(BUILD_DIR)/gpgpu-qwen-chat $(BUILD_DIR)/gpgpu-yolo
	rm -f $(KERNEL_HEADERS) $(KERNEL_BINARIES) $(BUILD_DIR)/model_eval.s
