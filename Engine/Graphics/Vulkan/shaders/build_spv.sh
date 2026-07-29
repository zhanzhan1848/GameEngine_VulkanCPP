#!/bin/bash
# build_spv.sh — dev tool for Tier 4 production Vulkan shaders.
#
# Compiles engine WGSL shaders (from Engine/Graphics/Dawn/shaders/) and
# hand-written GLSL shaders (colocated) into SPIR-V .spv files for use by
# the production ForwardRenderer via ShaderRegistry.
#
# Not part of CMake build. Re-run after touching any source shader:
#     ./build_spv.sh
#
# Requirements:
#   - naga  (cargo install naga-cli --locked)   — WGSL → SPIR-V
#   - glslangValidator (/usr/local/bin or brew) — GLSL → SPIR-V

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ENGINE_DAWN_DIR="../../Dawn/shaders"
DEST_DIR="."

# WGSL shaders consumed by ForwardRenderer (T4.1.3) + IBL precomputer (T4.0.4).
WGSL_SHADERS=(
    CameraDepth                  # depth pre-pass
    ShadowDepth                  # shadow pass + CSM
    DeferredLighting             # deferred lit pass
    DeferredLighting_Meshlet     # meshlet deferred
    GBuffer                      # gbuffer fill
    ForwardPBR                   # forward PBR main
    ForwardPBR_NoShadow          # forward PBR no-shadow variant
    BlurPass                     # VSM blur (compute)
    ToneMapping                  # tonemap
    Bloom                        # bloom
    IBL_BRDFIntegration          # IBL precompute
    IBL_IrradianceConvolution
    IBL_SpecularPrefilter
    IBL_EquirectangularToCube
    SSRPass                      # SSR (compute)
    SSRTrace
    SSRComposite
)

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
    src="$ENGINE_DAWN_DIR/$s.wgsl"
    if [ ! -f "$src" ]; then
        echo "MISS  $s.wgsl (skipped)"
        continue
    fi
    tmpdir="$(mktemp -d)"
    tmp="$tmpdir/$s.wgsl"
    inline_includes "$src" > "$tmp"
    if naga "$tmp" "$DEST_DIR/$s.spv" 2>"$tmpdir/err"; then
        sz=$(stat -f %z "$DEST_DIR/$s.spv")
        printf "OK    %-36s (%d bytes)\n" "$s.spv" "$sz"
    else
        echo "FAIL  $s.wgsl"
        sed 's/^/    /' "$tmpdir/err"
    fi
    rm -rf "$tmpdir"
done

echo
echo "Done."
