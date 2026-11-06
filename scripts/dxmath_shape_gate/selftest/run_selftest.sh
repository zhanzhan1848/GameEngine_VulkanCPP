#!/bin/bash
# ============================================================================
# 门禁自测（正例 + 四个负例 + 分类阴性对照）。
# 判定标准：
#   positive.cpp          → clang -fsyntax-only 零 error（exit 0）
#   negative_*.cpp        → 产生 error，且 classify_report.py 判为 MATH
# 任一不满足 → 非 0 退出并打印 FAIL 行。
# ============================================================================
set -u
GATE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$GATE_DIR/../.." && pwd)"
SELFTEST="$GATE_DIR/selftest"
REPORT_DIR="${DXMATH_GATE_REPORT_SELFTEST:-/tmp/dxmath_gate_selftest}"
CXX="${CXX:-clang++}"
export CLASSIFY_SCRIPT="$GATE_DIR/classify_report.py"
export GATE_DIR_OVERRIDE="$GATE_DIR"

FLAGS=(
  -std=c++20 -fsyntax-only -fms-extensions -fdeclspec
  -Wno-everything
  -include "$GATE_DIR/prefix/win_view_prefix.h"
  -I "$GATE_DIR/include"
  -I "$SELFTEST"
)

mkdir -p "$REPORT_DIR/per_tu"
FAIL=0

# 正例
"$CXX" "${FLAGS[@]}" "$SELFTEST/positive.cpp" > "$REPORT_DIR/positive.log" 2>&1
if [[ $? -eq 0 ]] && ! grep -q "error" "$REPORT_DIR/positive.log"; then
  echo "[PASS] positive.cpp 零错误"
else
  echo "[FAIL] positive.cpp 报错："; head -20 "$REPORT_DIR/positive.log"; FAIL=1
fi

# 负例：编译必须失败，且首错分类为 MATH
for neg in negative_columns negative_subscript negative_v4_from_v3 negative_broadcast; do
  log="$REPORT_DIR/$neg.log"
  "$CXX" "${FLAGS[@]}" "$SELFTEST/$neg.cpp" > "$log" 2>&1
  rc=$?
  if [[ $rc -eq 0 ]]; then
    echo "[FAIL] $neg.cpp 竟然编译通过（闸门失效）"; FAIL=1; continue
  fi
  # 分类：借用 classify_report.py 的块分类逻辑（单文件目录）
  slug_dir="$REPORT_DIR/per_tu"; rm -rf "$slug_dir"; mkdir -p "$slug_dir"
  cp "$log" "$slug_dir/selftest__${neg}.cpp.log"
  cls=$(python3 - "$slug_dir" <<'PYEOF'
import sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath("__file__")))
import importlib.util
spec = importlib.util.spec_from_file_location("cr", os.path.join(os.environ.get("GATE_DIR_OVERRIDE", ""), "classify_report.py"))
# 直接内联复用 classify()
src = open(os.environ["CLASSIFY_SCRIPT"]).read()
ns = {}
exec(src.split("def main")[0], ns)
d = sys.argv[1]
math_blocks = 0
for fn in os.listdir(d):
    lines = open(os.path.join(d, fn), errors="replace").read().splitlines()
    blocks, cur = [], []
    for ln in lines:
        if ns["DIAG_START"].match(ln):
            if cur: blocks.append(cur)
            cur = [ln]
        elif cur is not None:
            cur.append(ln)
    if cur: blocks.append(cur)
    for b in blocks:
        if "error" not in b[0]:
            continue
        if ns["classify"](b) == "MATH":
            math_blocks += 1
print(math_blocks)
PYEOF
)
  if [[ "$cls" -gt 0 ]]; then
    echo "[PASS] $neg.cpp 报 error 且分类为 MATH（$cls 块）"
  else
    echo "[FAIL] $neg.cpp 报 error 但未被分类为 MATH（分类器漏抓）"; head -8 "$log"; FAIL=1
  fi
done

exit $FAIL
