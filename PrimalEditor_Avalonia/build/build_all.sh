#!/usr/bin/env bash
# build_all.sh — single entry point for an Avalonia editor build.
# Runs the C++ engine build (EngineDLL + ContentTools) and then the
# Avalonia app bundle assembly. Use this instead of running the two
# scripts separately so a half-finished build can't leave you with
# a stale dylib in the editor's runtime directory.
#
# Usage: ./build_all.sh [configuration]   # default: DebugEditor
set -euo pipefail

CONFIG="${1:-DebugEditor}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "[build_all] step 1/2: native engine + content tools ($CONFIG)"
"$HERE/build_engine.sh" "$CONFIG"

echo "[build_all] step 2/2: Avalonia app bundle ($CONFIG)"
"$HERE/build_app.sh" "$CONFIG"

echo "[build_all] done — editor bundle is ready under Darwin/PrimalEditor_Avalonia/$CONFIG/"