#!/usr/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

export PATH=/usr/sbin:/usr/bin:/sbin:/bin

/usr/bin/busybox --install -s /usr/bin
set -e
mkdir -p /proc /sys /dev /mnt/gpgpu
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

modprobe 9pnet_virtio
mount -t 9p -o trans=virtio,version=9p2000.L,ro gpgpu_module /mnt/gpgpu

printf '\nARM64 GPGPU module test environment\n'
uname -r
printf 'Load:   insmod /mnt/gpgpu/gpgpu_pci.ko\n'
printf 'Unload: rmmod gpgpu_pci\n'
printf 'Exit:   poweroff -f\n\n'

while true; do
    setsid sh -i </dev/ttyAMA0 >/dev/ttyAMA0 2>&1 || sleep 1
done
