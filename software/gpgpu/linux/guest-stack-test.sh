#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

source_dir=/mnt/gpgpu-stack
output_dir=/mnt/gpgpu-build
stack_dir="$output_dir/stack"
test -d "$source_dir"
test -d "$stack_dir"

make -C "$source_dir" BUILD_DIR="$stack_dir" all
insmod "$output_dir/gpgpu_pci.ko"
test -c /dev/gpgpu0
test -d /sys/module/gpgpu_pci

"$stack_dir/gpgpu-demo" /dev/gpgpu0
"$stack_dir/test-runtime" /dev/gpgpu0

# 再次探测设备，确认测试中的映射和文件句柄已释放。
rmmod gpgpu_pci
test ! -e /dev/gpgpu0
insmod "$output_dir/gpgpu_pci.ko"
test -c /dev/gpgpu0
rmmod gpgpu_pci
test ! -d /sys/module/gpgpu_pci
printf 'GPGPU unload/reload: 2/2 PASS\n'
