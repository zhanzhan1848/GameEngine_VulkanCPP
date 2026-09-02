#!/usr/bin/env bash
# ci_local.sh — macOS 本地全门禁（.github/workflows/ci.yml 已移除 macOS job，
# 本脚本是它的替代：CLAUDE.md "本地已双配置全量验证" 的固化形式）。
#
# 用法: ./scripts/ci_local.sh [DebugEditor]
# 任一环节失败即非零退出。
set -euo pipefail

CONFIG="${1:-DebugEditor}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build-wt"
cd "$ROOT"

step() { printf '\n=== %s ===\n' "$1" >&2; }

step "1/6 C++ 构建（Engine + EngineDLL + PrimalEngineCli, ${CONFIG}）"
cmake -S "$ROOT" -B "$BUILD_DIR" >/dev/null   # CMAKE_SUPPRESS_REGENERATION ON：必须手动 configure
cmake --build "$BUILD_DIR" --target Engine --parallel >/dev/null
cmake --build "$BUILD_DIR" --target EngineDLL --parallel >/dev/null
cmake --build "$BUILD_DIR" --target PrimalEngineCli --parallel >/dev/null
cmake --build "$BUILD_DIR" --target vtbake --parallel >/dev/null

step "2/6 C# 构建（PrimalCli + PrimalEditor_Avalonia + Tests, ${CONFIG}）"
dotnet build PrimalCli/PrimalCli.csproj -c "${CONFIG}" --nologo -v q >/dev/null || fail "dotnet build PrimalCli"
dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c "${CONFIG}" --nologo -v q >/dev/null || fail "dotnet build editor"
dotnet build PrimalEditor_Avalonia.Tests/PrimalEditor_Avalonia.Tests.csproj -c "${CONFIG}" --nologo -v q >/dev/null || fail "dotnet build tests"

step "3/6 框架自测（primal selftest）"
"$ROOT/Darwin/PrimalCli/${CONFIG}/primal" selftest 2>&1 | grep -q "all passed" || fail "primal selftest"
echo "selftest OK"

step "4/8 CLI 单测（含 schema 快照门禁）"
(cd PrimalEditor_Avalonia.Tests && dotnet test -c "${CONFIG}" --no-build --filter "FullyQualifiedName~Cli" --nologo) \
    | grep -qE "已通过|Passed" || fail "CLI xunit tests"

step "5/8 离线引擎 CLI（engine info / render / cook-mesh）"
ENGINE="$ROOT/Darwin/${CONFIG}/primal-engine"
TMP="$(mktemp -d)"
KEEP_TMP=0
cleanup_tmp() { [ "$KEEP_TMP" = "1" ] || rm -rf "$TMP"; }
trap cleanup_tmp EXIT
fail() { KEEP_TMP=1; echo "FAIL: $1" >&2; exit 1; }

( cd "$TMP" && "$ENGINE" engine info ) 2>/dev/null | grep -q '"ok":true' || fail "engine info"
echo "engine info OK"
( cd "$TMP" && "$ENGINE" render --out "$TMP/r.png" --width 320 --height 180 --frames 2 ) >/dev/null 2>&1 \
    || fail "render"
[ -s "$TMP/r.png" ] || fail "render PNG 为空"
echo "render OK ($TMP/r.png)"

printf 'v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nf 1 2 3\nf 1 2 4\nf 1 3 4\nf 2 3 4\n' > "$TMP/t.obj"
( cd "$TMP" && "$ENGINE" cook-mesh --in "$TMP/t.obj" --out "$TMP/t.mesh" ) 2>/dev/null | grep -q '"ok":true' \
    || fail "cook-mesh"
[ -s "$TMP/t.mesh" ] || fail "cook-mesh 产物为空"
echo "cook-mesh OK"

step "6/8 渲染基线门禁（primal test render 双路径）"
PRIMAL="$ROOT/Darwin/PrimalCli/${CONFIG}/primal"
BASE="$TMP/baseline.png"
"$PRIMAL" test render --baseline "$BASE" >/dev/null 2>&1 || fail "test render 建基线"
"$PRIMAL" test render --baseline "$BASE" >/dev/null 2>&1 || fail "test render identical 应通过"
echo "baseline create + identical OK"

step "7/8 虚拟纹理烘焙（bake-vt 薄包装 vtbake）"
VTBAKE="$ROOT/build-wt/Tools/vtbake"
[ -x "$VTBAKE" ] || VTBAKE="$ROOT/Darwin/Debug/Tools/vtbake"
python3 -c "import zlib,struct;w=h=8;raw=b''.join(b'\\x00'+bytes([200,30,40]*w) for _ in range(h));chunk=lambda t,d:struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff);open('/tmp/ci_src.png','wb').write(b'\\x89PNG\\r\\n\\x1a\\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))"
( cd "$TMP" && PRIMAL_VTBAKE_BIN="$VTBAKE" "$ENGINE" bake-vt --in /tmp/ci_src.png --out "$TMP/t.vtex" ) \
    2>/dev/null | grep -q '"ok":true' || fail "bake-vt"
[ -s "$TMP/t.vtex" ] || fail "bake-vt 产物为空"
echo "bake-vt OK"

step "8/8 编辑器闭环冒烟（PRIMAL_OPEN_PROJECT；无显示环境用 SKIP_EDITOR_SMOKE=1 跳过）"
if [ "${SKIP_EDITOR_SMOKE:-0}" = "1" ]; then
    echo "skipped (SKIP_EDITOR_SMOKE=1)"
else
    python3 -c "
import zlib, struct
w = h = 8
raw = b''.join(b'\x00' + bytes([200, 30, 40] * w) for _ in range(h))
def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
open('$TMP/ci_src.png', 'wb').write(png)"
    "$ENGINE" bake-vt --in "$TMP/ci_src.png" --out "$TMP/ci_src.vtex" >/dev/null 2>&1 || fail "bake-vt 预检"
    PRIMAL_OPEN_PROJECT="$ROOT/projects_test/TestProject_01/TestProject_01.primal" \
        nohup "$ROOT/Darwin/PrimalEditor_Avalonia/${CONFIG}/PrimalEditor_Avalonia" > "$TMP/editor.log" 2>&1 &
    EDITOR_PID=$!
    for i in $(seq 1 48); do
        "$PRIMAL" editor.ping >/dev/null 2>&1 && break
        sleep 5
    done
    "$ROOT/scripts/primal_smoke.sh" "$TMP/editor_smoke" || fail "编辑器闭环冒烟"
    test -s "$ROOT/projects_test/TestProject_01/.Primal/logs/engine.log" || fail "FileSink 未落盘"
    kill -9 "$EDITOR_PID" 2>/dev/null || true
    echo "editor smoke OK"
fi

printf '\nCI LOCAL PASSED（%s）\n' "${CONFIG}" >&2
