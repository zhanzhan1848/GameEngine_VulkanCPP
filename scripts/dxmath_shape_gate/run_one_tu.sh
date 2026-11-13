#!/bin/bash
# 单 TU 门禁 worker（独立脚本，避免 export -f 跨 bash 版本的函数编码
# 不兼容问题——/bin/bash 3.2 导出的函数对 PATH 里的 bash 5 不可见）。
# 用法：run_one_tu.sh <tu 路径（相对仓库根或绝对）>
set -u
GATE_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$GATE_DIR/../.." && pwd)"
REPORT_DIR="${DXMATH_GATE_REPORT:-/tmp/dxmath_gate_report}"
PER_TU="$REPORT_DIR/per_tu"
CXX="${CXX:-clang++}"

DXSDK="$ROOT/third_party/xatlas/source/thirdparty/bgfx/3rdparty/dxsdk/include"

FLAGS=(
  -std=c++20 -fsyntax-only -fms-extensions -fdeclspec
  # clang 默认每 TU 报 20 个错就停（-ferror-limit=20）：公共头里的错误会吃满
  # 额度、掩盖 TU 自身的错误，使统计偏低。门禁要的是完整清单。
  -ferror-limit=0
  # 不加 -fno-rtti —— 引擎 Windows 构建开 RTTI（MSVC /GR，FreeList.h 的
  # typeid 在用）
  -Wno-everything
  -include "$GATE_DIR/prefix/win_view_prefix.h"
  -I "$GATE_DIR/include"
  -I "$ROOT"
  -I "$ROOT/Engine"
  -I "$ROOT/Engine/Common"
  -I "$ROOT/OSAPI/MAC"
  -I "$ROOT/third_party/stb"
  -I "$ROOT/third_party/VulkanMemoryAllocator/include"
  -I "$ROOT/third_party/moodycamel-ConcurrentQueue"
  -I "$ROOT/third_party/D3D12MemAlloc/include"
  -I "$ROOT/third_party/lua"
  -I "$ROOT/third_party/meshoptimizer/src"
  -I "$ROOT/ContentTools"
  -I "$ROOT/ContentTools/common"
  # 测试与第三方的包含根：缺了会触发 fatal error（找不到头），clang 随即
  # 停止分析，TU 里的数学错误被整体掩盖（首轮基线 46 个 TU 因此漏检）。
  -I "$ROOT/EngineTest"
  -I "$ROOT/EngineTest/UnitTests"
  -I "$ROOT/third_party/JoltPhysics"
  -I "$ROOT/third_party/astc-encoder/Source"
  -I "$ROOT/third_party/eigen"
  -I "$ROOT/third_party/libigl/include"
  -I "$DXSDK"
  -I /usr/local/include
  -I /opt/homebrew/include
  -DPRIMAL_PLUS=1 -D_LIB=1 -D_HAS_EXCEPTIONS=0
  -DENABLE_VULKAN=1 -DENABLE_D3D12=1
  -DPLATFORM_WINDOWS=1 -DCOMPILER_MSVC=1
  -DDEBUG=1 -D_DEBUG=1
  -DWIN32_LEAN_AND_MEAN=1
)

tu="$1"
[[ "$tu" != /* ]] && tu="$ROOT/$tu"
# ContentTools 以 C++17 构建（ContentTools/CMakeLists.txt）；其 vendored Eigen 在
# C++20 下用到已移除的 std::result_of/unary_negate，按 C++20 检查会出假错误。
if [[ "$tu" == "$ROOT/ContentTools/"* ]]; then
  FLAGS=("${FLAGS[@]/-std=c++20/-std=c++17}")
fi
slug_name="$(printf '%s' "$tu" | sed -e "s|^$ROOT/||" -e 's|/|__|g')"
log="$PER_TU/${slug_name}.log"

if [[ ! -f "$tu" ]]; then
  echo "MISSING_SOURCE $tu" > "$log"
  exit 0
fi

( ulimit -t 90 2>/dev/null; "$CXX" "${FLAGS[@]}" "$tu" ) > "$log" 2>&1
rc=$?
if [[ $rc -eq 124 || $rc -eq 152 ]]; then echo "TIMEOUT(rc=$rc)" >> "$log"; fi
exit 0
