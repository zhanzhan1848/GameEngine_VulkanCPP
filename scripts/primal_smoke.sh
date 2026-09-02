#!/usr/bin/env bash
# primal_smoke — CLI/Agent 接口的编辑闭环冒烟（Docs/CLI_Agent_Interface_Plan.md §8）。
#
# 前置：编辑器已启动且打开了一个项目（否则 scene.tree 返回 NOT_READY / exit 4）。
# 用法：./scripts/primal_smoke.sh [工作目录，默认 /tmp/primal_smoke]
# 退出码：任一步失败即非零（沿用 primal 退出码约定）。
set -euo pipefail

PRIMAL="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/primal.sh"
OUT_DIR="${1:-/tmp/primal_smoke}"
mkdir -p "$OUT_DIR"

step() { printf '\n=== %s ===\n' "$1" >&2; }
expect_ok() { # expect_ok <描述> <命令...>
    local desc="$1"; shift
    if ! "$@" > "$OUT_DIR/last.json" 2>"$OUT_DIR/last.err"; then
        echo "FAIL: $desc" >&2
        cat "$OUT_DIR/last.err" >&2
        exit 1
    fi
    echo "ok: $desc" >&2
}

step "连通性"
expect_ok "editor.ping" "$PRIMAL" --wait 10 editor.ping

step "场景查询"
expect_ok "scene.tree" "$PRIMAL" scene.tree

step "创建实体"
expect_ok "entity.create Sphere" "$PRIMAL" entity.create '{"kind":"Sphere","name":"SmokeSphere","position":[0,1,-4]}'
GUID="$(python3 -c 'import json;print(json.load(open("'"$OUT_DIR"'/last.json"))["data"]["entityGuid"])')"

step "修改变换"
expect_ok "entity.set-transform" "$PRIMAL" entity.set-transform "{\"entityGuid\":\"$GUID\",\"position\":[2,1,-4]}"

step "截图验证"
expect_ok "editor.screenshot" "$PRIMAL" editor.screenshot "{\"path\":\"$OUT_DIR/viewport.png\"}"
test -s "$OUT_DIR/viewport.png" || { echo "FAIL: 截图文件为空"; exit 1; }

step "帧遥测（metrics：均亮度/方差时间序列）"
expect_ok "editor.metrics" "$PRIMAL" editor.metrics '{"frames":5}'
python3 -c '
import json
d = json.load(open("'"$OUT_DIR"'/last.json"))
assert d["ok"] and d["data"]["frames"] == 5, d
print("   meanLuma 序列:", [round(s["meanLuma"], 4) for s in d["data"]["samples"]])'

step "帧序列捕获（record）"
expect_ok "editor.record" "$PRIMAL" editor.record "{\"frames\":5,\"out\":\"$OUT_DIR/record\"}"

step "帧间对比（diff + 热图）"
expect_ok "editor.diff" "$PRIMAL" editor.diff "{\"a\":\"$OUT_DIR/record/frame_0000.png\",\"b\":\"$OUT_DIR/record/frame_0004.png\",\"out\":\"$OUT_DIR/diff.png\"}"
python3 -c '
import json
d = json.load(open("'"$OUT_DIR"'/last.json"))
assert d["ok"] and d["data"]["sameSize"], d
print("   verdict:", d["data"]["verdict"], "| changed:", d["data"]["changedPixelCount"])'

step "撤销闭环（create 与 set-transform 各占一个动作：2 次 undo 移除，2 次 redo 恢复）"
"$PRIMAL" editor.undo >/dev/null 2>&1
expect_ok "editor.undo(2)" "$PRIMAL" editor.undo
"$PRIMAL" scene.tree 2>/dev/null | grep -q SmokeSphere && { echo "FAIL: undo 后实体仍在场景中"; exit 1; }
expect_ok "editor.redo(1)" "$PRIMAL" editor.redo
"$PRIMAL" scene.tree 2>/dev/null | grep -q SmokeSphere || { echo "FAIL: redo 后实体未恢复"; exit 1; }
expect_ok "editor.redo(2)" "$PRIMAL" editor.redo
"$PRIMAL" scene.tree 2>/dev/null | grep -q SmokeSphere || { echo "FAIL: 二次 redo 后实体未恢复"; exit 1; }
"$PRIMAL" editor.undo >/dev/null 2>&1
expect_ok "editor.undo(清理 2)" "$PRIMAL" editor.undo
"$PRIMAL" scene.tree 2>/dev/null | grep -q SmokeSphere && { echo "FAIL: 清理性 undo 后实体仍在"; exit 1; }

echo "SMOKE PASSED（产物在 ${OUT_DIR}）" >&2
