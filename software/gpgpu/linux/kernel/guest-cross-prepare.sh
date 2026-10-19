#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

# 输入是导入 ARM64 客体的源码与输出副本；只修复路径和构建时执行的工具。
# Alpine 的 /bin/sh 为 ash，支持 pipefail；不运行任何配置生成或内核构建目标。
set -eu
set -o pipefail
export LC_ALL=C

if test "$#" -gt 2; then
    printf '用法：%s [源码目录 [输出目录]]\n' "$0" >&2
    exit 1
fi
if test "$(uname -m)" != aarch64; then
    printf '必须在 ARM64 客体内使用本机工具链执行\n' >&2
    exit 1
fi
source_dir=$(cd "${1:-/root/gpgpu-kernel-source}" && pwd -P)
build_dir=$(cd "${2:-/root/gpgpu-kernel-output}" && pwd -P)
if test "$source_dir" = "$build_dir"; then
    printf '源码和输出必须是不同目录\n' >&2
    exit 1
fi
case "$source_dir$build_dir" in
    *[[:space:]]*) printf 'Kbuild 目录不能含空白字符\n' >&2; exit 1 ;;
esac
for program in gcc g++ make python3 flex bison cmp; do
    command -v "$program" >/dev/null
done
test -f "$source_dir/Makefile"
test -f "$build_dir/Makefile"
for required in .config Module.symvers include/config/auto.conf \
    include/config/kernel.release include/generated/autoconf.h \
    include/generated/rustc_cfg include/generated/utsrelease.h \
    scripts/module.lds scripts/mod/elfconfig.h scripts/mod/devicetable-offsets.h; do
    test -f "$build_dir/$required"
done

audit_dir=$build_dir/.guest-cross-prepare
mkdir -p "$audit_dir"
gcc --version > "$audit_dir/compiler.txt"
g++ --version > "$audit_dir/compiler-cxx.txt"

# *.cmd 是依赖和命令路径元数据，可以重定位；其余配置与生成文件按内容保护。
# 计算需要保持不变的配置、生成头和符号文件散列，排除可重定位的 .cmd。
protected_hashes()
{
    python3 - "$build_dir" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
names = [".config", "Module.symvers", "include/config", "include/generated",
         "arch/arm64/include/generated", "scripts/module.lds",
         "scripts/mod/elfconfig.h", "scripts/mod/devicetable-offsets.h"]
result = {}
for name in names:
    path = root / name
    if not path.exists():
        raise SystemExit(f"缺少受保护的开发文件：{path}")
    entries = sorted(path.rglob("*")) if path.is_dir() else [path]
    for entry in entries:
        if entry.name.endswith(".cmd") or entry.is_dir():
            continue
        key = str(entry.relative_to(root))
        result[key] = hashlib.sha256(entry.read_bytes()).hexdigest()
print(json.dumps(result, ensure_ascii=False, sort_keys=True, indent=2))
PY
}

protected_hashes > "$audit_dir/protected-before.json"
# 重新计算受保护文件散列，与准备前比较，发现变化即拒绝该开发副本。
check_protected()
{
    protected_hashes > "$audit_dir/protected-after.json"
    if ! cmp -s "$audit_dir/protected-before.json" "$audit_dir/protected-after.json"; then
        printf '配置、生成头或符号版本发生变化；禁止使用该开发副本\n' >&2
        return 1
    fi
}
# 保留原命令退出状态，并在所有退出路径核对受保护文件。
finish()
{
    result=$?
    trap - EXIT
    if ! check_protected; then
        result=1
    fi
    exit "$result"
}
# 即使某个工具编译失败，也核对受保护内容；失败时不输出 READY 标记。
trap finish EXIT

python3 - "$source_dir" "$build_dir" <<'PY'
import json
import os
from pathlib import Path
import re
import sys

source, output = map(Path, sys.argv[1:])
makefile = output / "Makefile"
text = makefile.read_text()
source_match = re.search(r"^include (.+)/Makefile$", text, re.M)
output_match = re.search(r"^export KBUILD_OUTPUT\s*[:?+]?=\s*(.+)$", text, re.M)
if not source_match or not output_match:
    raise SystemExit("无法识别交叉构建输出 Makefile 中的源码和输出路径")
old_source, old_output = source_match[1], output_match[1]
changes = sorted([(old_source, str(source)), (old_output, str(output))],
                 key=lambda pair: len(pair[0]), reverse=True)
rewritten = []
for path in [makefile, *sorted(output.rglob("*.cmd"))]:
    old = path.read_text()
    new = old
    for before, after in changes:
        new = new.replace(before, after)
    if new != old:
        path.write_text(new)
        rewritten.append(str(path.relative_to(output)))
link = output / "source"
if link.is_symlink():
    link.unlink()
elif link.exists():
    raise SystemExit("输出中的 source 不是符号链接，拒绝覆盖")
os.symlink(source, link)
(output / ".guest-cross-prepare/relocation.json").write_text(
    json.dumps({"old_source": old_source, "old_output": old_output,
                "source": str(source), "output": str(output),
                "rewritten_metadata": rewritten}, ensure_ascii=False,
               indent=2) + "\n")

# gcc 命令名可能与交叉构建时相同，不能靠 .cmd 判断宿主架构已经变化。
# 只移除工具目录中的对象和命令缓存，保留解析器源文件及目标生成头。
for name in ["basic", "kconfig", "mod", "genksyms", "gcc-plugins"]:
    directory = output / "scripts" / name
    for path in directory.glob("*"):
        if path.is_file() and (path.suffix == ".o" or path.name.endswith(".cmd")):
            path.unlink()
for name in ["basic/fixdep", "kconfig/conf", "mod/modpost", "mod/mk_elfconfig",
             "genksyms/genksyms", "gcc-plugins/latent_entropy_plugin.so",
             "sign-file", "sign-file.o", ".sign-file.o.cmd", ".sign-file.cmd"]:
    (output / "scripts" / name).unlink(missing_ok=True)
PY

# 直接进入工具子目录的 Kbuild，不经过顶层的 syncconfig/prepare。
# 交叉与本机编译器版本可以不同，已生成的功能配置仍以交叉产物为准。
unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS CROSS_COMPILE LLVM LLVM_IAS
unset KBUILD_OUTPUT KBUILD_EXTMOD KBUILD_EXTMOD_OUTPUT KCONFIG_CONFIG
# 直接调用工具子目录 Kbuild，只重建客体本机工具，不重新生成内核配置。
build_tools()
{
    make -C "$build_dir" -f "$source_dir/scripts/Makefile.build" -j2 \
        srctree="$source_dir" srcroot="$source_dir" objtree=. VPATH="$source_dir" \
        ARCH=arm64 SRCARCH=arm64 building_out_of_srctree=1 \
        CONFIG_SHELL=/bin/sh HOSTCC=gcc HOSTCXX=g++ CC=gcc LD=ld \
        HOSTPKG_CONFIG=pkg-config LEX=flex YACC=bison AWK=mawk \
        KBUILD_HOSTCFLAGS="-O2 -Wall -D_FILE_OFFSET_BITS=64 -I $source_dir/scripts/include" \
        KBUILD_HOSTCXXFLAGS="-O2 -Wall -I $source_dir/scripts/include" \
        "$@"
}

build_tools obj=scripts/basic scripts/basic/fixdep
build_tools obj=scripts/kconfig scripts/kconfig/conf
# 两个头描述目标 ARM64 的布局，不能按宿主工具的构建流程重新生成。
build_tools obj=scripts/mod -o scripts/mod/elfconfig.h \
    -o scripts/mod/devicetable-offsets.h scripts/mod/modpost scripts/mod/mk_elfconfig
tool_list='scripts/basic/fixdep scripts/kconfig/conf scripts/mod/modpost scripts/mod/mk_elfconfig'
if grep -qx 'CONFIG_GENKSYMS=y' "$build_dir/.config"; then
    build_tools obj=scripts/genksyms scripts/genksyms/genksyms
    tool_list="$tool_list scripts/genksyms/genksyms"
fi
if grep -qx 'CONFIG_MODULE_SIG_FORMAT=y' "$build_dir/.config"; then
    build_tools obj=scripts scripts/sign-file
    tool_list="$tool_list scripts/sign-file"
fi
if grep -Eq '^CONFIG_GCC_PLUGIN_(RANDSTRUCT|STACKLEAK)=y$' "$build_dir/.config"; then
    printf '本脚本尚未处理该配置中的其他 GCC 插件，不能保留宿主插件继续使用\n' >&2
    exit 1
fi
if grep -qx 'CONFIG_GCC_PLUGIN_LATENT_ENTROPY=y' "$build_dir/.config"; then
    plugin_headers=$(gcc -print-file-name=plugin)/include
    test -f "$plugin_headers/gcc-plugin.h"
    # 插件版本标记来自同一源码；仅查询版本，不触发 Kconfig。
    kernel_version=$(make -s -C "$source_dir" ARCH=arm64 kernelversion)
    build_tools obj=scripts/gcc-plugins GCC_PLUGIN=latent_entropy_plugin.so \
        KERNELVERSION="$kernel_version" scripts/gcc-plugins/latent_entropy_plugin.so
    tool_list="$tool_list scripts/gcc-plugins/latent_entropy_plugin.so"
    # 真正让客体 GCC 加载插件，检查架构及本机 GCC 插件接口是否匹配。
    gcc -fplugin="$build_dir/scripts/gcc-plugins/latent_entropy_plugin.so" \
        -x c -c /dev/null -o "$audit_dir/plugin-load.o"
fi

# 列表只含固定的无空格相对路径；ELF e_machine=183 表示 AArch64。
# shellcheck disable=SC2086
python3 - "$build_dir" $tool_list <<'PY'
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
result = {}
for name in sys.argv[2:]:
    data = (root / name).read_bytes()
    if (data[:6] != b"\x7fELF\x02\x01" or
            int.from_bytes(data[18:20], "little") != 183):
        raise SystemExit(f"工具不是 ARM64 小端 ELF：{name}")
    result[name] = "AArch64"
(root / ".guest-cross-prepare/tools.json").write_text(
    json.dumps(result, indent=2) + "\n")
PY

check_protected
trap - EXIT
printf 'GPGPU_CROSS_TOOLS_READY=%s\n' "$(cat "$build_dir/include/config/kernel.release")"
