#!/usr/bin/env bash
# Native Metal comparison input for the frozen Vulkan geometry-visibility run.
# Does not claim GI algorithm parity; see the local alignment topic document.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
export PRIMAL_TEST_MODEL=SponzaTangentFrames.model
export PRIMAL_METAL_ALBEDO_MIPS=1
export PRIMAL_METAL_SC_QUALIFIED="${PRIMAL_METAL_SC_QUALIFIED:-1}"
export SPGI_CAM_LOCK=1
export PRIMAL_SHADOW_PCSS=1
export PRIMAL_METAL_HIGH=1
export PRIMAL_SPGI=1
# Use the existing depth-history weighting; geometry-measured mode 7 is separate.
export SPGI_DDGI_VISIBILITY="${SPGI_DDGI_VISIBILITY:-2}"
# Native Fusion returns linear HDR; the shared host still defaults Metal to
# its legacy LDR passthrough. Select the existing HDR entry, without an EV tweak.
export PRIMAL_TONE_ACES=1
export PRIMAL_METAL_EYE_ADAPTATION="${PRIMAL_METAL_EYE_ADAPTATION:-1}"
export SPONZA_MAX_FRAMES="${SPONZA_MAX_FRAMES:-400}"
export SPONZA_CAPTURE_FRAME="${SPONZA_CAPTURE_FRAME:-350}"
# Explicit selection supports backend-isolated CMake output directories.
binary="${PRIMAL_METAL_BINARY:-$root/Darwin/Debug/TestModularPipeline}"
[[ -x "$binary" ]] || { printf 'Metal test executable unavailable: %s\n' "$binary" >&2; exit 2; }
exec "$binary" "$@"
