#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# 客体内运行示例：1. 构建用户库与示例（Kernel 头文件已由宿主机生成在 stack/）；
# 2. 装入固件与驱动；3. 运行示例。示例从设备能力读取 GPUVM 模式。
set -eu
test "$#" -eq 0
source_dir=/mnt/gpgpu-stack
output_dir=/mnt/gpgpu-build
stack_dir="$output_dir/stack"
mkdir -p "$stack_dir"
make -C "$source_dir" BUILD_DIR="$stack_dir" all
if test -L /lib/firmware && ! test -e /lib/firmware; then
    unlink /lib/firmware
fi
mkdir -p /lib/firmware
cp "$output_dir/gpgpu-cp.bin" /lib/firmware/gpgpu-cp.bin
insmod "$output_dir/gpgpu_pci.ko"
test -c /dev/dri/renderD128
"$stack_dir/gpgpu-demo" /dev/dri/renderD128
