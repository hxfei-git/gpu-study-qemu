#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# 只在客体的独立源码副本中注入返回值，不修改正式模块或新增用户接口。
set -eu
if test -L /lib/firmware && ! test -e /lib/firmware; then
    unlink /lib/firmware
fi
mkdir -p /lib/firmware
cp /mnt/gpgpu-build/gpgpu-cp.bin /lib/firmware/gpgpu-cp.bin
source_dir=/root/gpgpu-module-src
fault_source=/root/gpgpu-fault-src
mkdir -p "$fault_source"
cp "$source_dir"/*.c "$source_dir"/*.h "$source_dir/Makefile" "$fault_source/"
kernel_dir=
for directory in /usr/src/linux-headers-*-virt; do
    test -s "$directory/Module.symvers" || continue
    kernel_dir=$directory
done
test -n "$kernel_dir"
for mode in map unmap invalidate; do
    cp "$source_dir/gpgpu_queue.c" "$source_dir/gpgpu_gpuvm.c" "$fault_source/"
    if test "$mode" = invalidate; then
        awk '{ print; if ($0 ~ /status = readl\(c->dev->regs \+ GPGPU_REG_VM_STATUS\)/) print "\tif (c->vmid == 1) status = 0;"; }' \
            "$source_dir/gpgpu_gpuvm.c" > "$fault_source/gpgpu_gpuvm.c"
    else
        opcode=GPGPU_MAP_QUEUES
        result=GPGPU_MANAGEMENT_PENDING
        if test "$mode" = unmap; then
            opcode=GPGPU_UNMAP_QUEUES
            result=7
        fi
        awk -v opcode="$opcode" -v result="$result" '{ print; if ($0 ~ /\*status = le32_to_cpu\(READ_ONCE\(request->result->status\)\)/) print "\t\t\tif (kind == GPGPU_QUEUE_USER_AQL && opcode == " opcode ") *status = " result ";"; }' \
            "$source_dir/gpgpu_queue.c" > "$fault_source/gpgpu_queue.c"
    fi
    make -C "$fault_source" KDIR="$kernel_dir" BUILD_DIR="/root/gpgpu-fault-build-$mode"
    cp "/root/gpgpu-fault-build-$mode/gpgpu_pci.ko" "/mnt/gpgpu-build/gpuvm/retention/fault-$mode.ko"
    cp "$fault_source/gpgpu_queue.c" "/mnt/gpgpu-build/gpuvm/retention/queue-$mode.c"
    cp "$fault_source/gpgpu_gpuvm.c" "/mnt/gpgpu-build/gpuvm/retention/vm-$mode.c"
    insmod "/root/gpgpu-fault-build-$mode/gpgpu_pci.ko" queue_mode=2
    /mnt/gpgpu-build/gpuvm-stack/test-gpuvm "$mode"
    /mnt/gpgpu-build/gpuvm-stack/test-gpuvm retained
    rmmod gpgpu_pci
    printf 'PASS gpuvm-retain-%s\n' "$mode"
done
