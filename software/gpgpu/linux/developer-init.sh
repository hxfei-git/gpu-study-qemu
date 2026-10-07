#!/usr/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

export PATH=/usr/sbin:/usr/bin:/sbin:/bin

/usr/bin/busybox --install -s /usr/bin
set -eu

developer_root=/mnt/developer-root
mkdir -p /proc /sys /dev "$developer_root"
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

modprobe virtio_blk
modprobe ext4
mount -t ext4 /dev/vda "$developer_root"

mkdir -p "$developer_root/proc" "$developer_root/sys" "$developer_root/dev"
mount --move /proc "$developer_root/proc"
mount --move /sys "$developer_root/sys"
mount --move /dev "$developer_root/dev"

exec switch_root "$developer_root" /usr/sbin/gpgpu-dev-init
