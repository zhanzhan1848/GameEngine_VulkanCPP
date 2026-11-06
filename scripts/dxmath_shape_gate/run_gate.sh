#!/bin/bash
# ============================================================================
# DirectXMath 形状门禁运行器。
#
# 对 tu_list.txt 中每个编译单元（或 --tu 指定的单个 TU）在"Windows 视角"
# （prefix/win_view_prefix.h：标准头按 Apple 配置预解析 → #undef __APPLE__
# → _WIN32/_WIN64/_MSC_VER）下执行 clang++ -std=c++20 -fsyntax-only。
# 单 TU 的实际编译在 run_one_tu.sh（独立 worker，避免 export -f 跨
# bash 版本的函数编码不兼容）。
#
# 诊断二分类（classify_report.py）：
#   MATH     数学相关（DirectXMath/XMFLOAT 形状、MathAccess/MathDirectXOps、
#            columns/下标/构造/运算符不匹配且涉数学类型）
#   PLATFORM 平台/垫片相关（其余）
#
# 输出：/tmp/dxmath_gate_report/（per_tu/*.log + summary.txt）
# 用法：run_gate.sh [--tu <path>] [--jobs N]
# ============================================================================
set -u
GATE_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$GATE_DIR/../.." && pwd)"
LIST="$GATE_DIR/tu_list.txt"
REPORT_DIR="${DXMATH_GATE_REPORT:-/tmp/dxmath_gate_report}"
PER_TU="$REPORT_DIR/per_tu"
PARALLEL="${DXMATH_GATE_JOBS:-8}"
ONLY_TU=""

usage() {
  echo "用法: $0 [--tu <编译单元路径（相对仓库根或绝对）>] [--jobs N]"
  echo "      环境变量: DXMATH_GATE_REPORT（默认 /tmp/dxmath_gate_report）"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --tu) ONLY_TU="$2"; shift 2 ;;
    --jobs) PARALLEL="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "未知参数: $1" >&2; usage; exit 2 ;;
  esac
done

mkdir -p "$PER_TU"

if [[ -n "$ONLY_TU" ]]; then
  DXMATH_GATE_REPORT="$REPORT_DIR" "$GATE_DIR/run_one_tu.sh" "$ONLY_TU"
  slug_name="$(printf '%s' "$ONLY_TU" | sed -e "s|^$ROOT/||" -e 's|/|__|g')"
  cat "$PER_TU/${slug_name}.log"
  exit 0
fi

# 批量执行（awk+xargs；macOS /bin/bash 3.2 无 mapfile）
TUS_FILE="$(mktemp)"
awk -F'\t' 'NF>=2 && $1 !~ /^#/ {print $1}' "$LIST" > "$TUS_FILE"
N_TUS=$(wc -l < "$TUS_FILE" | tr -d ' ')
# bash 3.2 会把紧跟变量的多字节字符首字节并进变量名，必须 ${} 定界
echo "门禁 TU 数: ${N_TUS}（并行 ${PARALLEL}）"
xargs -P "$PARALLEL" -I {} "$GATE_DIR/run_one_tu.sh" "{}" < "$TUS_FILE"
rm -f "$TUS_FILE"

# ---------------------------------------------------------------------------
# 汇总 + 分类
# ---------------------------------------------------------------------------
python3 "$GATE_DIR/classify_report.py" "$REPORT_DIR" || exit 3

if [[ -f "$REPORT_DIR/summary.txt" ]]; then
  echo
  echo "汇总写入 $REPORT_DIR/summary.txt"
else
  echo "!! summary.txt 未生成（分类器失败）" >&2
  exit 3
fi
