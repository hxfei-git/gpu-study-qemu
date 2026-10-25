#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

# 在已挂载的软件栈中执行一种正常验收；每种模式保留自己的加载和卸载步骤。
set -eu
mode=${1:?需要 sync、queue 或 gpuvm 模式}
test "$#" -eq 1
source_dir=/mnt/gpgpu-stack
output_dir=/mnt/gpgpu-build

case "$mode" in
    sync)
        stack_dir="$output_dir/stack"
        test -d "$source_dir"
        test -d "$stack_dir"
        ;;
    queue|gpuvm)
        stack_dir="$output_dir/$mode-stack"
        # 开发盘没有 Live ISO 的 modloop 挂载，将悬空固件链接换成本地目录。
        if test -L /lib/firmware && ! test -e /lib/firmware; then
            unlink /lib/firmware
        fi
        mkdir -p /lib/firmware
        if test "$mode" = queue; then
            mkdir -p "$stack_dir"
        fi
        ;;
    *)
        printf '未知验收模式：%s\n' "$mode" >&2
        exit 1
        ;;
esac

make -C "$source_dir" BUILD_DIR="$stack_dir" all
if test "$mode" != sync; then
    cp "$output_dir/gpgpu-cp.bin" /lib/firmware/gpgpu-cp.bin
fi

case "$mode" in
    sync)
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
        ;;
    queue)
        insmod "$output_dir/gpgpu_pci.ko" queue_mode=1
        test -c /dev/gpgpu0
        "$stack_dir/test-queue"
        # 最后一组故障用例结束后，设备移除负责停止 CP 和执行器。
        rmmod gpgpu_pci
        test ! -e /dev/gpgpu0
        printf 'GPGPU queue unload: 1/1 PASS\n'
        ;;
    gpuvm)
        insmod "$output_dir/gpgpu_pci.ko" queue_mode=2
        test -c /dev/dri/renderD128
        "$stack_dir/test-gpuvm"
        rmmod gpgpu_pci
        test ! -e /dev/dri/renderD128
        printf 'GPUVM unload: PASS\n'
        ;;
esac
