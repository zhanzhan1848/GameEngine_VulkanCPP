#!/usr/bin/env bash
# build_run.sh — launch the freshly-built editor binary for the current
# platform. Designed to be called from the editor's Run button after a
# successful build.
#
# Usage: ./build_run.sh [config]   # default: DebugEditor
set -euo pipefail

CONFIG="${1:-DebugEditor}"
REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

UNAME="$(uname -s 2>/dev/null || echo Linux)"

case "$UNAME" in
    Darwin)
        APP="$REPO_ROOT/Darwin/PrimalEditor_Avalonia/$CONFIG/PrimalEditor.app"
        if [[ -d "$APP" ]]; then
            echo "[build_run] open '$APP'"
            open "$APP"
        else
            echo "[build_run] $APP not found; falling back to raw executable" >&2
            "$REPO_ROOT/Darwin/PrimalEditor_Avalonia/$CONFIG/PrimalEditor_Avalonia"
        fi
        ;;
    Linux)
        BIN="$REPO_ROOT/Linux/PrimalEditor_Avalonia/$CONFIG/PrimalEditor_Avalonia"
        if [[ -x "$BIN" ]]; then
            echo "[build_run] '$BIN'"
            "$BIN" &
        else
            echo "[build_run] executable not found: $BIN" >&2
            exit 1
        fi
        ;;
    MINGW*|CYGWIN*|MSYS*)
        BIN="$REPO_ROOT/Windows/PrimalEditor_Avalonia/$CONFIG/PrimalEditor_Avalonia.exe"
        if [[ -x "$BIN" ]]; then
            echo "[build_run] start '$BIN'"
            cmd //c start "" "$(cygpath -w "$BIN")" 2>/dev/null || "$BIN" &
        else
            echo "[build_run] executable not found: $BIN" >&2
            exit 1
        fi
        ;;
    *)
        echo "[build_run] unsupported platform: $UNAME" >&2
        exit 1
        ;;
esac
