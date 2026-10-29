#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

stage_name=${1:?Expected a staging directory name}
test "$#" -eq 1
case "$stage_name" in
    .|..|*/*)
        printf 'Expected a single staging directory name\n' >&2
        exit 1
        ;;
esac

output_dir="/mnt/gpgpu-build/$stage_name"
test -d "$output_dir"
shared_stack=/mnt/gpgpu-stack
shared_source="$shared_stack/linux"
if ! test -d "$shared_stack/include"; then
    mkdir -p "$shared_stack"
    mount -t 9p -o trans=virtio,version=9p2000.L,ro \
        gpgpu_stack "$shared_stack"
fi
# 客体内保留与源码树相同的 linux/ 与 include/ 相对布局，Kbuild 的
# -I$(src)/../include 才能找到共享协议头。
module_root=/root/gpgpu-module-src
module_source="$module_root/linux"
module_build=/root/gpgpu-module-build
object_file="$module_build/gpgpu_pci.o"
mkdir -p "$module_source" "$module_build"

# sync_file() - 内容不同才复制，内容相同时保留时间戳以免重复编译。
changed_files=0
sync_file() {
    source_file=$1
    destination=$2
    if cmp -s "$source_file" "$destination"; then
        return
    fi
    mkdir -p "${destination%/*}"
    cp "$source_file" "$destination"
    changed_files=$((changed_files + 1))
}
for source_file in "$shared_source"/Makefile "$shared_source"/*.c "$shared_source"/*.h; do
    sync_file "$source_file" "$module_source/${source_file##*/}"
done
for source_file in "$shared_stack"/include/gpgpu/*.h "$shared_stack"/include/gpgpu/uapi/*.h; do
    sync_file "$source_file" "$module_root/${source_file#"$shared_stack"/}"
done
printf 'GPGPU_SOURCE_CHANGED=%s\n' "$changed_files"

kernel_dir=
for directory in /usr/src/linux-headers-*-virt; do
    test -d "$directory" && test -s "$directory/Module.symvers" || continue
    if test -n "$kernel_dir"; then
        printf 'Expected exactly one kernel development directory\n' >&2
        exit 1
    fi
    kernel_dir=$directory
done
if test -z "$kernel_dir"; then
    printf 'Matching kernel development files are missing\n' >&2
    exit 1
fi
kernel_release=${kernel_dir##*/linux-headers-}
printf 'GPGPU_TARGET_RELEASE=%s\n' "$kernel_release"

if test -f "$object_file"; then
    printf 'GPGPU_OBJECT_MTIME_BEFORE=%s\n' \
        "$(stat -c '%Y:%y' "$object_file")"
else
    printf 'GPGPU_OBJECT_MTIME_BEFORE=missing\n'
fi
make -C "$module_source" KDIR="$kernel_dir" BUILD_DIR="$module_build"
test -s "$object_file"
test -s "$module_build/gpgpu_pci.ko"
printf 'GPGPU_OBJECT_MTIME=%s\n' "$(stat -c '%Y:%y' "$object_file")"
cp "$module_build/gpgpu_pci.ko" "$output_dir/gpgpu_pci.ko"
sync
