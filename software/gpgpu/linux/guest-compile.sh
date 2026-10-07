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
shared_source=/mnt/gpgpu-src
module_source=/root/gpgpu-module-src
module_build=/root/gpgpu-module-build
object_file="$module_build/gpgpu_pci.o"
mkdir -p "$module_source" "$module_build"

changed_files=0
for name in Makefile gpgpu_pci.c gpgpu_regs.h; do
    if cmp -s "$shared_source/$name" "$module_source/$name"; then
        continue
    fi
    # Use guest timestamps; retain existing timestamps for unchanged content.
    cp "$shared_source/$name" "$module_source/$name"
    changed_files=$((changed_files + 1))
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
