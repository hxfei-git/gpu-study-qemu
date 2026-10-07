#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

source_dir=/mnt/gpgpu-src
output_dir=/mnt/gpgpu-build
runtime_dir="$output_dir/${1:?Expected a staging directory name}"
test -d "$runtime_dir"

# The bootstrap system lives in tmpfs; leave room for development packages.
mount -o remount,size=75% /
ip link set eth0 up
udhcpc -i eth0 -n -q
printf '%s\n' /media/vda/apks \
    https://dl-cdn.alpinelinux.org/alpine/v3.24/main \
    https://dl-cdn.alpinelinux.org/alpine/v3.24/community \
    > /etc/apk/repositories

# Alpine live media supplies a read-only modules directory.
if test -L /lib/modules; then
    live_modules=$(readlink -f /lib/modules)
    unlink /lib/modules
    mkdir /lib/modules
    for directory in "$live_modules"/*; do
        ln -s "$directory" "/lib/modules/${directory##*/}"
    done
fi

mkdir -p "$output_dir/apk-cache"
# Resolve both packages from the same repository index. Old APK versions
# are removed from mirrors, so do not pin only the development package.
apk --cache-dir "$output_dir/apk-cache" add \
    build-base linux-virt linux-virt-dev mkinitfs e2fsprogs

kernel_dir=
for directory in /usr/src/linux-headers-*-virt; do
    test -d "$directory" || continue
    if test -n "$kernel_dir"; then
        printf 'Expected exactly one kernel development directory\n' >&2
        exit 1
    fi
    kernel_dir=$directory
done
test -n "$kernel_dir"
test -s "$kernel_dir/Module.symvers"
kernel_release=${kernel_dir##*/linux-headers-}

# Save the compiler environment before building the experimental module.
# A driver compile error must not discard the installed kernel headers.
cp /boot/vmlinuz-virt "$runtime_dir/vmlinuz-virt"
cp "$kernel_dir/Module.symvers" "$runtime_dir/kernel-Module.symvers"
cp "$kernel_dir/.config" "$runtime_dir/kernel-config"

printf '%s\n' 'kernel/fs/9p/*' 'kernel/net/9p/*' \
    > /etc/mkinitfs/features.d/gpgpu9p.modules
mkinitfs -i "$source_dir/module-init.sh" -F 'base virtio gpgpu9p' \
    -o "$runtime_dir/initramfs-module" "$kernel_release"
mkinitfs -i "$source_dir/developer-init.sh" \
    -F 'base virtio ext4 gpgpu9p' \
    -o "$runtime_dir/initramfs-developer" "$kernel_release"
printf '%s\n' "$kernel_release" > "$runtime_dir/kernel-release"
apk info -v linux-virt linux-virt-dev > "$runtime_dir/kernel-packages"

# /dev/vdb is the new, dedicated image attached by the prepare command.
test -f "$runtime_dir/developer.raw"
test -b /dev/vdb
sectors=$(cat /sys/class/block/vdb/size)
test "$((sectors * 512))" -eq "$(stat -c %s "$runtime_dir/developer.raw")"
mkfs.ext4 -F /dev/vdb
developer_root=/mnt/developer-root
mkdir -p "$developer_root"
mount -t ext4 /dev/vdb "$developer_root"
cp -a /bin /sbin /lib /usr /etc /root /var "$developer_root/"
mkdir -p "$developer_root/proc" "$developer_root/sys" \
    "$developer_root/dev" "$developer_root/tmp" "$developer_root/run" \
    "$developer_root/mnt/gpgpu-src" "$developer_root/mnt/gpgpu-build"
chmod 1777 "$developer_root/tmp"
cp "$source_dir/guest-dev-init.sh" \
    "$developer_root/usr/sbin/gpgpu-dev-init"
chmod 755 "$developer_root/usr/sbin/gpgpu-dev-init"
sync
umount "$developer_root"
