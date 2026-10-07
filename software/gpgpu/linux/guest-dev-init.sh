#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

export PATH=/usr/sbin:/usr/bin:/sbin:/bin
set -eu

mkdir -p /mnt/gpgpu-src /mnt/gpgpu-build
modprobe 9pnet_virtio
mount -t 9p -o trans=virtio,version=9p2000.L,ro \
    gpgpu_src /mnt/gpgpu-src
mount -t 9p -o trans=virtio,version=9p2000.L \
    gpgpu_build /mnt/gpgpu-build

printf '\nGPGPU_DEVELOPER_READY\n'

while true; do
    setsid sh -i </dev/ttyAMA0 >/dev/ttyAMA0 2>&1 || sleep 1
done
