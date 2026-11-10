#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# 客体模型环境：挂载固定材料与输入，在本机编译后端、装载驱动并进入应用 shell。
set -eu

source_dir=/mnt/gpgpu-stack
build_dir=/mnt/gpgpu-build
stack_dir="$build_dir/stack"

mkdir -p "$source_dir" /mnt/gpgpu-models /mnt/gpgpu-input /mnt/gpgpu-output
mountpoint -q "$source_dir" || mount -t 9p -o trans=virtio,version=9p2000.L,ro \
    gpgpu_stack "$source_dir"
mountpoint -q /mnt/gpgpu-models || mount -t 9p -o trans=virtio,version=9p2000.L,ro \
    gpgpu_models /mnt/gpgpu-models
mountpoint -q /mnt/gpgpu-input || mount -t 9p -o trans=virtio,version=9p2000.L,ro \
    gpgpu_input /mnt/gpgpu-input
mountpoint -q /mnt/gpgpu-output || mount -t 9p -o trans=virtio,version=9p2000.L \
    gpgpu_output /mnt/gpgpu-output

make -C "$source_dir" BUILD_DIR="$stack_dir" all
if test -L /lib/firmware && ! test -e /lib/firmware; then
    unlink /lib/firmware
fi
mkdir -p /lib/firmware
cp "$build_dir/gpgpu-cp.bin" /lib/firmware/gpgpu-cp.bin
if ! test -d /sys/module/gpgpu_pci; then
    insmod "$build_dir/gpgpu_pci.ko" vram_clear_dma=1
fi
test -c /dev/dri/renderD128

export PATH="$stack_dir:$PATH"
export GPGPU_MODELS_DIR=/mnt/gpgpu-models
export GPGPU_MODEL_OUTPUT=/mnt/gpgpu-output
export OPENBLAS_NUM_THREADS=1
export OMP_NUM_THREADS=1
printf '\n客体模型环境已就绪。\n'
printf '聊天：gpgpu-qwen-chat\n'
printf '检测：gpgpu-yolo --image /mnt/gpgpu-input/bus.jpg\n'
printf '输入、分词、图像前后处理均在本客体执行；退出客体使用 poweroff -f。\n\n'
exec sh -i
