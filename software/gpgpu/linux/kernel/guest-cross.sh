#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

# 只在独立开发盘安装交叉产物；内核功能配置及生成头由交叉构建固定。
set -eu
# Alpine 的 /bin/sh 为 BusyBox ash。
# shellcheck disable=SC3040
set -o pipefail
jobs=${1:?需要客体并发数}
shared=/mnt/gpgpu-build
materials=/mnt/gpgpu-src/kernel
source_dir=/root/gpgpu-kernel-source
build_dir=/root/gpgpu-kernel-output
SOURCE_DATE_EPOCH=$(cat "$shared/source-date-epoch")
export SOURCE_DATE_EPOCH

set --
while IFS= read -r package; do
    set -- "$@" "$shared/apk-cache/$package"
done < "$shared/build-dependencies"
apk --no-network --cache-dir "$shared/apk-cache" add "$@"
apk info -v | grep -x 'gcc-15.2.0-r5' > /dev/null
apk info -v | sort > "$shared/build-packages"
gcc --version > "$shared/guest-compiler-version"

fingerprint=$(cat "$shared/cross-import.sha256")
if ! test -f "$source_dir/.gpgpu-cross-input" || \
    test "$(cat "$source_dir/.gpgpu-cross-input")" != "$fingerprint"; then
    for directory in "$source_dir" "$build_dir"; do
        if test -d "$directory"; then
            mv "$directory" "$directory.previous.$(date +%s)"
        fi
    done
    zstd -dc "$shared/cross-development.tar.zst" | tar -xf - -C /root
    printf '%s\n' "$fingerprint" > "$source_dir/.gpgpu-cross-input"
fi
release=$(cat "$shared/kernel-release")
test "$release" = 6.18.57-0-gpgpu-virt
# 该目录只保存本配方安装的内核模块，避免重建时留下已移除的旧模块。
rm -rf "/lib/modules/$release/kernel"
zstd -dc "$shared/cross-modules.tar.zst" | tar -xf - -C /

# ARM64 客体不能执行 x86_64 host-tools；只替换工具和路径元数据。
sh "$materials/guest-cross-prepare.sh" "$source_dir" "$build_dir"
depmod -a "$release"
sh "$materials/guest-export.sh" "$jobs" cross "$release"
