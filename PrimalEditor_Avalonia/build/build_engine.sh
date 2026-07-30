#!/usr/bin/env bash
# build_engine.sh — invoke the C++ engine build via cmake and stream
# stdout/stderr to the caller (the editor's Process.Start with stdout-redirect).
# Repo root is discovered via `git rev-parse --show-toplevel`.
#
# Builds EngineDLL AND ContentTools (the Avalonia editor loads
# libContentTools.dylib at runtime; skipping it silently breaks CookMesh).
#
# Usage:
#   ./build_engine.sh                 [DebugEditor build]
#   ./build_engine.sh rebuild         [clean + build]
#   ./build_engine.sh [config]        [explicit build config]
set -euo pipefail

ACTION="${1:-build}"
case "$ACTION" in
    rebuild) CONFIG="${2:-DebugEditor}"; DO_CLEAN=1 ;;
    *)       CONFIG="$ACTION";            DO_CLEAN=0 ;;
esac

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
BUILD_DIR="$REPO_ROOT/build-wt"

if [[ ! -d "$BUILD_DIR" ]]; then
    echo "build-wt/ not found at $BUILD_DIR — run cmake once to generate it." >&2
    exit 1
fi

cd "$BUILD_DIR"

if [[ "$DO_CLEAN" == "1" ]]; then
    echo "[build_engine] clean (cmake --build . --target clean)"
    cmake --build . --target clean 2>&1 || true
fi

echo "[build_engine] cmake --build . --target EngineDLL --target ContentTools --config $CONFIG"
cmake --build . --target EngineDLL --target ContentTools --config "$CONFIG" 2>&1
echo "[build_engine] done"
