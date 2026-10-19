#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# 在现有开发客体中编译用户库并运行独立 GPUVM 验收。
set -eu
output_dir=/mnt/gpgpu-build
if test -L /lib/firmware && ! test -e /lib/firmware; then
    unlink /lib/firmware
fi
mkdir -p /lib/firmware
make -C /mnt/gpgpu-stack BUILD_DIR="$output_dir/gpuvm-stack" all
cp "$output_dir/gpgpu-cp.bin" /lib/firmware/gpgpu-cp.bin
insmod "$output_dir/gpgpu_pci.ko" queue_mode=2
test -c /dev/dri/renderD128
"$output_dir/gpuvm-stack/test-gpuvm"
rmmod gpgpu_pci
test ! -e /dev/dri/renderD128
printf 'GPUVM unload: PASS\n'
