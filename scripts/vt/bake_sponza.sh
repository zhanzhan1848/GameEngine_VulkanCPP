#!/bin/bash
# bake_sponza.sh — VT M1.8:Sponza 全套纹理烘焙(75 张,VTex 双格式)
# 用法: scripts/vt/bake_sponza.sh [输出目录](默认 /tmp/sponza_vt)
# 依赖: build/Tools/vtbake(根 CMake -DBUILD_VTBAKE=ON,默认开)
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VTBAKE="$ROOT/build/Tools/vtbake"
SRC_DIR="$ROOT/EngineTest/assets/models/Sponza"
OUT_DIR="${1:-/tmp/sponza_vt}"

if [ ! -x "$VTBAKE" ]; then
    echo "vtbake not found at $VTBAKE — build with: cmake --build build --target vtbake" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
total_src=0
total_vt=0
count=0
fail=0

for png in "$SRC_DIR"/*.png; do
    name="$(basename "$png" .png)"
    out="$OUT_DIR/$name.vtex"
    if "$VTBAKE" "$png" "$out" --kind auto > /tmp/vtbake_one.log 2>&1; then
        sz=$(stat -f %z "$out"); total_vt=$((total_vt + sz))
        src_sz=$(stat -f %z "$png"); total_src=$((total_src + src_sz))
        count=$((count + 1))
    else
        echo "FAIL: $name" >&2
        cat /tmp/vtbake_one.log >&2
        fail=$((fail + 1))
    fi
done

echo "=================================================="
echo "baked: $count / $((count + fail))  (failures: $fail)"
echo "source PNG total: $total_src bytes"
echo "VTEX total:       $total_vt bytes"
[ "$fail" -eq 0 ]
