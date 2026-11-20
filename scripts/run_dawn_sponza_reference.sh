#!/usr/bin/env bash
# Matched inputs for the native Dawn host, not full Vulkan pipeline parity.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
export PRIMAL_TEST_MODEL=SponzaTangentFrames.model
export SPGI_CAM_LOCK=1
export SPONZA_MAX_FRAMES="${SPONZA_MAX_FRAMES:-400}"
export SPONZA_CAPTURE_FRAME="${SPONZA_CAPTURE_FRAME:-350}"
binary="${PRIMAL_DAWN_BINARY:-$root/Darwin/RelWithDebInfo/TestDawnSponza}"
[[ -x "$binary" ]] || { printf 'Dawn test executable unavailable: %s\n' "$binary" >&2; exit 2; }
exec "$binary" "$@"
