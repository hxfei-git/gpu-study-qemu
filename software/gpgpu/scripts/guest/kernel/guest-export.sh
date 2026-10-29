#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

# 输入为客体并发数、构建方式和版本；所有导出都来自同一套已安装内核。
set -eu
# Alpine /bin/sh 固定为 BusyBox ash，管道任一环节失败都必须停止导出。
# shellcheck disable=SC3040
set -o pipefail
jobs=${1:?需要客体并发数}
mode=${2:?需要构建方式}
release=${3:?需要内核版本}
shared=/mnt/gpgpu-build
build_dir=/root/gpgpu-kernel-output

# 开发盘同时保留完整源码和 O= 输出；构建链接指向同一套生成文件。
for old in /usr/src/linux-headers-*-virt; do
    test -e "$old" || continue
    test "$old" != "/usr/src/linux-headers-$release" || continue
    mv "$old" "$old.distribution"
done
ln -sfn "$build_dir" "/usr/src/linux-headers-$release"
ln -sfn "$build_dir" "/lib/modules/$release/build"
if test "$mode" = native; then
    cp "$build_dir/arch/arm64/boot/Image" "$shared/vmlinuz-virt"
fi
cp "$build_dir/.config" "$shared/kernel-config"
cp "$build_dir/Module.symvers" "$shared/kernel-Module.symvers"
printf '%s\n' "$release" > "$shared/kernel-release"
printf 'Linux %s; gpu_buddy built-in; %s build\n' "$release" "$mode" \
    > "$shared/kernel-packages"
printf '%s\n' 'kernel/fs/9p/*' 'kernel/net/9p/*' \
    > /etc/mkinitfs/features.d/gpgpu9p.modules
mkinitfs -i /mnt/gpgpu-src/developer-init.sh -F 'base virtio ext4 gpgpu9p' \
    -o "$shared/initramfs-developer" "$release"

# 可独立搬走的模块包和开发文件。私钥保留在原构建输出，不放进导出包。
tar -C / -cf - "lib/modules/$release" | zstd -T"$jobs" -q \
    -o "$shared/kernel-modules.tar.zst" -f
tar -C /root --exclude='*.o' --exclude='*.a' --exclude='*.ko' \
    --exclude='vmlinux*' --exclude='Image*' --exclude='signing_key.pem' \
    -cf - gpgpu-kernel-source gpgpu-kernel-output | \
    zstd -T"$jobs" -q -o "$shared/kernel-development.tar.zst" -f
sync
printf 'GPGPU_KERNEL_ARTIFACTS_READY=%s\n' "$release"
