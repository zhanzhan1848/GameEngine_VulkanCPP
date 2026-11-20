#!/usr/bin/env bash
# Small native shader regression; no historical capture or Dawn SDK required.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
build="${PRIMAL_NATIVE_TEST_BUILD_DIR:-$root/Darwin/NativeRegression}"
mkdir -p "$build/swift-cache"
exec xcrun swift -module-cache-path "$build/swift-cache" \
  "$root/scripts/tests/metal_hzb_mips.swift" \
  "$root/Engine/Graphics/Metal/shaders/HZBGeneration.metal"
