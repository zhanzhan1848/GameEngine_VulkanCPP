#!/usr/bin/env bash
# primal — CLI / Agent 客户端便捷入口（Docs/CLI_Agent_Interface_Plan.md Phase 0）。
#
# 用法:
#   ./scripts/primal.sh editor.ping
#   ./scripts/primal.sh editor.schema
#   ./scripts/primal.sh scene.tree
#   ./scripts/primal.sh editor.screenshot '{"path":"/tmp/viewport.png"}'
#
# 前置: 编辑器（PrimalEditor_Avalonia）正在运行。--wait N 可等待其启动。
# 退出码: 0 成功 / 2 参数错 / 3 资源不存在 / 4 引擎内部错 / 5 编辑器未运行。
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# 找最新一次构建出的 primal 宿主可执行（DebugEditor 优先，其次 Debug/Release*）。
EXE=""
for cfg in DebugEditor Debug ReleaseEditor Release; do
    for ext in "" ".dll"; do
        candidate="$REPO_ROOT/Darwin/PrimalCli/$cfg/primal$ext"
        if [[ -f "$candidate" ]]; then
            EXE="$candidate"
            break 2
        fi
    done
done

if [[ -z "$EXE" ]]; then
    echo "primal: 未找到已构建的客户端，先执行:" >&2
    echo "  dotnet build $REPO_ROOT/PrimalCli/PrimalCli.csproj -c DebugEditor" >&2
    exit 4
fi

case "$EXE" in
    *.dll)
        exec dotnet "$EXE" "$@"
        ;;
    *)
        exec "$EXE" "$@"
        ;;
esac
