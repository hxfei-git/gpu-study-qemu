#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

source_dir=/mnt/gpgpu-src
output_dir=/mnt/gpgpu-build
runtime_dir="$output_dir/${1:?需要暂存目录名}"
test -d "$runtime_dir"

# 引导系统在 tmpfs 中；APK 来自固定清单，保留签名验证。
mount -o remount,size=75% /
if test -L /lib/modules; then
    live_modules=$(readlink -f /lib/modules)
    unlink /lib/modules
    mkdir /lib/modules
    for directory in "$live_modules"/*; do
        ln -s "$directory" "/lib/modules/${directory##*/}"
    done
fi
set --
while IFS= read -r package; do
    set -- "$@" "$output_dir/apk-cache/$package"
done < "$runtime_dir/bootstrap-packages"
apk --no-network --cache-dir "$output_dir/apk-cache" add "$@"
apk info -v | grep -x 'gcc-15.2.0-r5' > /dev/null

kernel_dir=
for directory in /usr/src/linux-headers-*-virt; do
    test -d "$directory" || continue
    if test -n "$kernel_dir"; then
        printf '必须只有一个内核开发目录\n' >&2
        exit 1
    fi
    kernel_dir=$directory
done
test -n "$kernel_dir"
test -s "$kernel_dir/Module.symvers"
kernel_release=${kernel_dir##*/linux-headers-}
test "$kernel_release" = 6.18.57-0-virt

# 只保存基础开发环境，不编译驱动或旧同步栈。
cp /boot/vmlinuz-virt "$runtime_dir/vmlinuz-virt"
cp "$kernel_dir/Module.symvers" "$runtime_dir/kernel-Module.symvers"
cp "$kernel_dir/.config" "$runtime_dir/kernel-config"

printf '%s\n' 'kernel/fs/9p/*' 'kernel/net/9p/*' \
    > /etc/mkinitfs/features.d/gpgpu9p.modules
mkinitfs -i "$source_dir/developer-init.sh" \
    -F 'base virtio ext4 gpgpu9p' \
    -o "$runtime_dir/initramfs-developer" "$kernel_release"
printf '%s\n' "$kernel_release" > "$runtime_dir/kernel-release"
apk info -v linux-virt linux-virt-dev > "$runtime_dir/kernel-packages"

# /dev/vdb 仅为本次引导新建的专用空盘；格式化前核对大小。
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
