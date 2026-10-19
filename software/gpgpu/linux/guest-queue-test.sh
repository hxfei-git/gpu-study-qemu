#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
source_dir=/mnt/gpgpu-stack
output_dir=/mnt/gpgpu-build
stack_dir="$output_dir/queue-stack"
# 开发盘没有 Live ISO 的 modloop 挂载，将悬空固件链接换成本地目录。
if test -L /lib/firmware && ! test -e /lib/firmware; then
    unlink /lib/firmware
fi
mkdir -p "$stack_dir" /lib/firmware
make -C "$source_dir" BUILD_DIR="$stack_dir" all
cp "$output_dir/gpgpu-cp.bin" /lib/firmware/gpgpu-cp.bin
insmod "$output_dir/gpgpu_pci.ko" queue_mode=1
test -c /dev/gpgpu0
"$stack_dir/test-queue"
# 正常卸载或最终故障用例结束后，设备移除负责停止 CP 和执行器。
rmmod gpgpu_pci
test ! -e /dev/gpgpu0
printf 'GPGPU queue unload: 1/1 PASS\n'
