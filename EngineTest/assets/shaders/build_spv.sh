#!/bin/bash
# build_spv.sh — dev tool for Tier 3 Vulkan parity tests.
#
# Compiles engine WGSL shaders (from Engine/Graphics/Dawn/shaders/) and
# hand-written GLSL shaders (colocated) into SPIR-V .spv files for use by
# EngineTest/UnitTests/RHI/Platforms/Vulkan/* parity tests.
#
# Not part of CMake build. Re-run after touching any source shader:
#     ./build_spv.sh
#
# Requirements:
#   - naga  (cargo install naga-cli --locked)   — WGSL → SPIR-V
#   - glslangValidator (/usr/local/bin or brew) — GLSL → SPIR-V

set -e

# Resolve paths relative to this script so it can be run from anywhere.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ENGINE_SHADERS_DIR="../../../Engine/Graphics/Dawn/shaders"
DEST_DIR="."

# WGSL shaders to compile from the engine shader directory. Keep this list
# in sync with which Tier 3 sub-phase needs each shader.
WGSL_SHADERS=(
    CameraDepth           # T3.1 DepthPrePass
    ShadowDepth           # T3.2 ShadowPass + CSM
    BlurPass              # T3.3 VSM blur (compute)
    ToneMapping           # T3.4 ToneMap
    Bloom                 # T3.4 Bloom bright-pass extraction
    IBL_Hammersley        # T3.5 IBL helper (no includes)
    IBL_BRDFIntegration   # T3.5 BRDF LUT  (includes Hammersley)
    IBL_IrradianceConvolution  # T3.5
    IBL_SpecularPrefilter      # T3.5
    IBL_EquirectangularToCube  # T3.5
    SSRPass               # T3.6 SSR (compute)
    SSRTrace              # T3.6
    SSRComposite          # T3.6
)

# Inline `#include "X.wgsl"` directives. One level deep — sufficient for
# engine shaders which only include IBL_Hammersley.wgsl from IBL_*.wgsl.
inline_includes() {
    local src="$1"
    local dir
    dir="$(dirname "$src")"
    awk -v dir="$dir" '
        /^#include[[:space:]]*"[^"]+".*$/ {
            match($0, /"[^"]+"/)
            inc = substr($0, RSTART+1, RLENGTH-2)
            path = dir "/" inc
            while ((getline line < path) > 0) print line
            close(path)
            next
        }
        { print }
    ' "$src"
}

echo "==> WGSL (naga) ===="
for s in "${WGSL_SHADERS[@]}"; do
    src="$ENGINE_SHADERS_DIR/$s.wgsl"
    if [ ! -f "$src" ]; then
        echo "MISS  $s.wgsl (skipped)"
        continue
    fi
    # mktemp -d and put a fixed-name file inside; macOS mktemp -t would put
    # the random suffix after our .wgsl extension and confuse naga's input
    # format detection.
    tmpdir="$(mktemp -d)"
    tmp="$tmpdir/$s.wgsl"
    inline_includes "$src" > "$tmp"
    if naga "$tmp" "$DEST_DIR/$s.spv" 2>"$tmpdir/err"; then
        sz=$(stat -f %z "$DEST_DIR/$s.spv")
        printf "OK    %-32s (%d bytes)\n" "$s.spv" "$sz"
    else
        echo "FAIL  $s.wgsl"
        sed 's/^/    /' "$tmpdir/err"
    fi
    rm -rf "$tmpdir"
done

echo
echo "==> GLSL (glslangValidator) ===="
# Hand-written GLSL shaders in this directory (layout-critical shaders that
# must match C++ struct truth and therefore can't be auto-translated).
for stage in vert frag comp; do
    for src in "$DEST_DIR"/*."$stage"; do
        [ -f "$src" ] || continue
        [ "$src" = "$DEST_DIR/pc_triangle.vert" ] && continue   # legacy demo shader
        [ "$src" = "$DEST_DIR/pc_triangle.frag" ] && continue
        [ "$src" = "$DEST_DIR/pc_triangle_v2.vert" ] && continue
        [ "$src" = "$DEST_DIR/triangle.vert" ] && continue
        [ "$src" = "$DEST_DIR/triangle.frag" ] && continue
        [ "$src" = "$DEST_DIR/ubo_triangle.vert" ] && continue
        [ "$src" = "$DEST_DIR/ubo_triangle.frag" ] && continue
        out="${src%.$stage}.$stage.spv"
        if glslangValidator -V --aml -e main -o "$out" "$src" 2>"$src.err"; then
            sz=$(stat -f %z "$out")
            printf "OK    %-32s (%d bytes)\n" "$(basename "$out")" "$sz"
            rm -f "$src.err"
        else
            echo "FAIL  $(basename "$src")"
            cat "$src.err" 2>/dev/null | sed 's/^/    /'
        fi
    done
done

echo
echo "Done."
