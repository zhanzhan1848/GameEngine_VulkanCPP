#!/bin/bash
# Apple 视角语法检查（与 Windows 视角的 run_gate.sh 配对使用）。
#
# 用 Darwin/Debug 构建树里该编译单元所属目标的 flags.make（CXX_DEFINES /
# CXX_INCLUDES / CXX_FLAGS，与真实 Mac 构建逐字相同）跑 -fsyntax-only，
# 不写 .o、不动构建树，可多个代理并发使用。
#
# 用法：run_apple_syntax.sh <tu 路径（相对仓库根或绝对）> [构建树，默认 Darwin/Debug]
# 退出码：0 = 无错误；1 = 有编译错误；2 = 该 TU 不在 Apple 构建中（Windows 专属等）
set -u
GATE_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$GATE_DIR/../.." && pwd)"
BUILD="${2:-$ROOT/Darwin/Debug}"
[[ "$BUILD" != /* ]] && BUILD="$ROOT/$BUILD"

tu="$1"
[[ "$tu" != /* ]] && tu="$ROOT/$tu"
if [[ ! -f "$tu" ]]; then
  echo "MISSING_SOURCE $tu" >&2
  exit 2
fi

# 找到引用该源文件的目标（build.make 里以绝对路径列出源文件）
build_make="$(grep -rlF --include=build.make "$tu" "$BUILD" 2>/dev/null | head -1)"
if [[ -z "$build_make" ]]; then
  echo "NOT_IN_APPLE_BUILD $tu（$BUILD 中没有目标编译它）"
  exit 2
fi
flags_make="$(dirname "$build_make")/flags.make"

read_var() { sed -n "s/^$1 = //p" "$flags_make" | head -1; }
DEFINES="$(read_var CXX_DEFINES)"
INCLUDES="$(read_var CXX_INCLUDES)"
CXXFLAGS="$(read_var CXX_FLAGS)"
CXX="$(sed -n 's/^# compile CXX with //p' "$flags_make" | head -1)"
CXX="${CXX:-/usr/bin/c++}"

# -w：只看错误（真实构建的警告与本检查无关，避免淹没错误）
# shellcheck disable=SC2086  # 有意按空白拆分（与 make 展开一致）
eval "\"$CXX\" $DEFINES $INCLUDES $CXXFLAGS -fsyntax-only -ferror-limit=0 -w \"$tu\""
rc=$?
[[ $rc -ne 0 ]] && exit 1
exit 0
