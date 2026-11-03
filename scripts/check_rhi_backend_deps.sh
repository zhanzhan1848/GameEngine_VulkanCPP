#!/usr/bin/env bash
# check_rhi_backend_deps.sh — RHI 后端依赖静态门禁（D3D12 M0 F0-12）
#
# 规则（Master Plan §3 契约 1：无按后端的调用方代码分支）：
#   共享代码（Engine/ 除后端目录、EngineDLL/、EngineTest/）禁止：
#     P1  include 任何 RHI 后端目录头：  .../Platforms/(Vulkan|Metal|Dawn|D3D12)/...
#     P2  static_cast 到任何后端类型：    static_cast<.../(Vulkan|Metal|Dawn|D3D12)...>
#   豁免域（后端自身与 legacy 后端目录——它们 cast/include 自己是合法的）：
#     Engine/Graphics/RHI/Platforms/   Engine/Graphics/Metal|Vulkan|Direct3D12|Dawn/
#   已登记违规：scripts/rhi_backend_deps_whitelist.txt
#     格式 = 路径|分类|理由|登记日期；分类 ∈ {平台宿主, 待逐步消化, 后端专属实验, 后端专属测试}
#     白名单按"文件"粒度登记；注释行不扫描（// 与 /* 块内 * 行）。
#
# 退出码：0=通过（无未登记违规）；1=存在未登记违规；2=脚本自身错误。
# 用法：./scripts/check_rhi_backend_deps.sh [-v]（-v 打印全部违规明细）
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WHITELIST="$ROOT/scripts/rhi_backend_deps_whitelist.txt"
VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

[ -f "$WHITELIST" ] || { echo "FATAL: whitelist not found: $WHITELIST" >&2; exit 2; }

# 扫描文件集（Engine + EngineDLL + EngineTest；豁免后端/legacy 目录）
MAPFILE_TMP="$(mktemp)"
trap 'rm -f "$MAPFILE_TMP"' EXIT
find "$ROOT/Engine" "$ROOT/EngineDLL" "$ROOT/EngineTest" \
     \( -name '*.h' -o -name '*.cpp' -o -name '*.mm' \
        -o -name '*.inl' -o -name '*.hpp' \) 2>/dev/null \
 | grep -v -E "^$ROOT/Engine/Graphics/RHI/Platforms/" \
 | grep -v -E "^$ROOT/Engine/Graphics/(Metal|Vulkan|Direct3D12|Dawn)/" \
 > "$MAPFILE_TMP"
[ -s "$MAPFILE_TMP" ] || { echo "FATAL: scan set is empty" >&2; exit 2; }

# 违规行（P1 include / P2 static_cast），过滤注释行，路径仓库相对化
VIOLATIONS="$(xargs grep -Hn -E \
    '^[[:space:]]*#[[:space:]]*include[[:space:]]*"[^"]*Platforms/(Vulkan|Metal|Dawn|D3D12)/|static_cast<[^>]*(Vulkan|Metal|Dawn|D3D12)' \
    < "$MAPFILE_TMP" 2>/dev/null \
  | grep -v -E ':[0-9]+:[[:space:]]*(//|\*|/\*)' \
  | sed "s|^$ROOT/||" || true)"

VIOLATING_FILES="$(printf '%s\n' "$VIOLATIONS" | awk -F: 'NF>=3{print $1}' | sort -u)"
[ -z "$VIOLATIONS" ] && VIOLATING_FILES=""

# 白名单路径集合（忽略空行与 # 注释行）
WHITELISTED="$(grep -v -E '^[[:space:]]*(#|$)' "$WHITELIST" | awk -F'|' '{print $1}' | sed "s|^$ROOT/||" | sort -u)"

UNLISTED="$(comm -23 <(printf '%s\n' "$VIOLATING_FILES") <(printf '%s\n' "$WHITELISTED") | grep -v '^$' || true)"
STALE="$(comm -13 <(printf '%s\n' "$VIOLATING_FILES") <(printf '%s\n' "$WHITELISTED") | grep -v '^$' || true)"

total_files=$(printf '%s\n' "$VIOLATING_FILES" | grep -c . || true)
total_lines=$(printf '%s\n' "$VIOLATIONS" | grep -c . || true)
unlisted_n=0; [ -n "$UNLISTED" ] && unlisted_n=$(printf '%s\n' "$UNLISTED" | grep -c .)
stale_n=0; [ -n "$STALE" ] && stale_n=$(printf '%s\n' "$STALE" | grep -c .)

echo "=== RHI backend dependency gate (F0-12) ==="
echo "scan set:      $(wc -l < "$MAPFILE_TMP" | tr -d ' ') files"
echo "violations:    $total_lines lines in $total_files files (all whitelisted categories)"
if [ "$VERBOSE" = 1 ] && [ -n "$VIOLATIONS" ]; then
    printf '%s\n' "$VIOLATIONS"
fi
if [ -n "$STALE" ]; then
    echo "stale whitelist entries (no current violation — consider pruning):" >&2
    printf '%s\n' "$STALE" | sed 's/^/  WARN: /' >&2
fi
if [ "$unlisted_n" -gt 0 ]; then
    echo "UNWHITELISTED violations ($unlisted_n):" >&2
    printf '%s\n' "$UNLISTED" | sed 's/^/  FAIL: /' >&2
    echo "RHI backend dependency gate: FAILED" >&2
    exit 1
fi
echo "RHI backend dependency gate: PASSED (0 unlisted; $stale_n stale whitelist entries)"
exit 0
