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
    build-base linux-virt linux-virt-dev mkinitfs

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

# Build on guest tmpfs to avoid host/guest timestamp skew over 9P.
module_build=$(mktemp -d /tmp/gpgpu-module.XXXXXX)
make -C "$source_dir" KDIR="$kernel_dir" BUILD_DIR="$module_build"
cp "$module_build/gpgpu_pci.ko" "$runtime_dir/gpgpu_pci.ko"
cp /boot/vmlinuz-virt "$runtime_dir/vmlinuz-virt"
cp "$kernel_dir/Module.symvers" "$runtime_dir/kernel-Module.symvers"
cp "$kernel_dir/.config" "$runtime_dir/kernel-config"

printf '%s\n' 'kernel/fs/9p/*' 'kernel/net/9p/*' \
    > /etc/mkinitfs/features.d/gpgpu9p.modules
mkinitfs -i "$source_dir/module-init.sh" -F 'base virtio gpgpu9p' \
    -o "$runtime_dir/initramfs-module" "$kernel_release"
printf '%s\n' "$kernel_release" > "$runtime_dir/kernel-release"
modinfo "$runtime_dir/gpgpu_pci.ko"
apk info -v linux-virt linux-virt-dev > "$runtime_dir/kernel-packages"
sync
