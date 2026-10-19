#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

# 输入为 CPU 并发数；源码、输出和安装均只修改专用构建盘。
set -eu
jobs=${1:?需要编译并发数}
shared=/mnt/gpgpu-build
materials=/mnt/gpgpu-src/kernel
source_dir=/root/gpgpu-kernel-source
build_dir=/root/gpgpu-kernel-output

# Host 已按固定版本和散列准备依赖；客体只安装本地包，不重新解析镜像版本。
set --
while IFS= read -r package; do
    set -- "$@" "$shared/apk-cache/$package"
done < "$shared/build-dependencies"
apk --no-network --cache-dir "$shared/apk-cache" add "$@"
apk info -v | grep -qx 'gcc-15.2.0-r5'
apk info -v | sort > "$shared/build-packages"
gcc --version > "$shared/compiler-version"

fingerprint=$(cat "$shared/source.sha256")
if ! test -f "$source_dir/.gpgpu-input" || \
    test "$(cat "$source_dir/.gpgpu-input")" != "$fingerprint"; then
    # 每套新输入使用新的源码和输出；失败重试保留相同输入的中间产物。
    if test -d "$source_dir"; then
        mv "$source_dir" "$source_dir.previous.$(date +%s)"
        if test -d "$build_dir"; then
            mv "$build_dir" "$build_dir.previous.$(date +%s)"
        fi
    fi
    mkdir -p "$source_dir" "$build_dir"
    printf '正在向独立构建盘展开源码\n'
    tar -xf "$shared/source.tar" -C "$source_dir" --strip-components=1
    printf '%s\n' "$fingerprint" > "$source_dir/.gpgpu-input"
fi

export KBUILD_BUILD_USER=gpgpu KBUILD_BUILD_HOST=alpine-aarch64
export KBUILD_BUILD_VERSION=1
export SOURCE_DATE_EPOCH
SOURCE_DATE_EPOCH=$(cat "$shared/source-date-epoch")
KBUILD_BUILD_TIMESTAMP=$(date -u -d "@$SOURCE_DATE_EPOCH" '+%Y-%m-%d %H:%M:%S')
export KBUILD_BUILD_TIMESTAMP
unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS

if ! test -f "$build_dir/.config"; then
    cp "$materials/virt.aarch64.config" "$build_dir/.config"
    cat "$materials/gpgpu.config" >> "$build_dir/.config"
fi
make -C "$source_dir" O="$build_dir" ARCH=arm64 olddefconfig
grep -qx 'CONFIG_GPU_BUDDY=y' "$build_dir/.config"
grep -qx 'CONFIG_MODULE_SIG_KEY="certs/signing_key.pem"' "$build_dir/.config"
make -C "$source_dir" O="$build_dir" ARCH=arm64 AWK=mawk -j"$jobs" Image modules
release=$(make -s -C "$source_dir" O="$build_dir" kernelrelease)
test "$release" = 6.18.57-0-gpgpu-virt
grep -q '[[:space:]]gpu_buddy_init[[:space:]]' "$build_dir/Module.symvers"
make -C "$source_dir" O="$build_dir" ARCH=arm64 \
    INSTALL_MOD_STRIP=1 modules_install

sh "$materials/guest-export.sh" "$jobs" native "$release"
