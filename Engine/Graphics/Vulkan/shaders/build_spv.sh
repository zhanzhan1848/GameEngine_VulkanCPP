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

# Nanite subdir (T4.4.1+) — WGSL shaders in Dawn/shaders/Nanite/, output to Vulkan/shaders/Nanite/
NANITE_SHADERS=(
    HZBCopy                      # HZB base mip copy (compute, depth-tex read)
    HZBMip                       # HZB mip reduction (compute, color-tex read)
    ShadowBlit                   # D32 → R32 depth blit (compute, depth-tex read)
    GlobalSDFVoxelization        # T4.4.2: SDF voxelization (3D storage image write)
    ShadowDepth                  # T4.4.3: shadow depth vertex shader (vertex pulling)
    GPUCullingPipeline           # T4.4.4: 8-stage GPU culling (compute, multi-entry)
    GPUDrivenDraw                # T4.4.5: meshlet draw VS+FS (vertex pulling, MRT)
    ShadowCulling                # T4.6.5 part 17.1: shadow cluster culling (compute, 2 entries)
)

echo
echo "==> Nanite WGSL (naga) ===="
mkdir -p "$DEST_DIR/Nanite"
for s in "${NANITE_SHADERS[@]}"; do
    src="$ENGINE_DAWN_DIR/Nanite/$s.wgsl"
    if [ ! -f "$src" ]; then
        echo "MISS  Nanite/$s.wgsl (skipped)"
        continue
    fi
    tmpdir="$(mktemp -d)"
    tmp="$tmpdir/$s.wgsl"
    inline_includes "$src" > "$tmp"
    if naga "$tmp" "$DEST_DIR/Nanite/$s.spv" 2>"$tmpdir/err"; then
        sz=$(stat -f %z "$DEST_DIR/Nanite/$s.spv")
        printf "OK    Nanite/%-30s (%d bytes)\n" "$s.spv" "$sz"
    else
        echo "FAIL  Nanite/$s.wgsl"
        sed 's/^/    /' "$tmpdir/err"
    fi
    rm -rf "$tmpdir"
done

echo
echo "==> Hand-written GLSL (glslangValidator) ===="
# T4.6.5 part 16.3+: Particle + Line shaders are hand-written GLSL (no WGSL
# source). Compile each .vert/.frag pair to .spv. Format: rel path without
# extension → adds .vert/.frag and outputs .vert.spv/.frag.spv next to source.
# Bash 3.2 (macOS default) lacks associative arrays — use parallel arrays.
GLSL_NAMES=( "Particle/Particle" "Forward/Line" )
for pair in "${GLSL_NAMES[@]}"; do
    src_dir="$(dirname "$pair")"
    src_name="$(basename "$pair")"
    for stage in vert frag; do
        src="$DEST_DIR/$src_dir/$src_name.$stage"
        if [ ! -f "$src" ]; then
            echo "MISS  $src_dir/$src_name.$stage (skipped)"
            continue
        fi
        out="$DEST_DIR/$src_dir/$src_name.$stage.spv"
        if glslangValidator -V "$src" -o "$out" 2>/dev/null; then
            sz=$(stat -f %z "$out")
            printf "OK    %-36s (%d bytes)\n" "$src_dir/$src_name.$stage.spv" "$sz"
        else
            echo "FAIL  $src_dir/$src_name.$stage"
        fi
    done
done

# T4.6.5 part 17.2+: Nanite hand-written GLSL. Each is a single-stage compute
# or a combined vert+frag (VisibilityBuffer) that needs spirv-link to merge.
# Compute: simple glslangValidator call with --source-entrypoint main -e NAME.
# VisibilityBuffer: compile vert+frag separately, then spirv-link into one .spv.
echo
echo "==> Nanite hand-written GLSL ===="

# ClusterBinning (compute, entry cluster_binning_kernel)
src="$DEST_DIR/Nanite/ClusterBinning.comp"
if [ -f "$src" ]; then
    out="$DEST_DIR/Nanite/ClusterBinning.spv"
    if glslangValidator -V --source-entrypoint main -e cluster_binning_kernel "$src" -o "$out" 2>/dev/null; then
        sz=$(stat -f %z "$out"); printf "OK    Nanite/ClusterBinning.spv          (%d bytes)\n" "$sz"
    else echo "FAIL  Nanite/ClusterBinning.comp"; fi
else echo "MISS  Nanite/ClusterBinning.comp (skipped)"; fi

# VisibilityBuffer (combined vert+frag, 2 entries)
vb_dir="$DEST_DIR/Nanite"
if [ -f "$vb_dir/VisibilityBuffer.vert" ] && [ -f "$vb_dir/VisibilityBuffer.frag" ]; then
    tmpdir="$(mktemp -d)"
    if glslangValidator -V --source-entrypoint main -e visibility_vertex_shader \
           "$vb_dir/VisibilityBuffer.vert" -o "$tmpdir/vb.vert.spv" 2>/dev/null && \
       glslangValidator -V --source-entrypoint main -e visibility_fragment_shader \
           "$vb_dir/VisibilityBuffer.frag" -o "$tmpdir/vb.frag.spv" 2>/dev/null && \
       spirv-link "$tmpdir/vb.vert.spv" "$tmpdir/vb.frag.spv" -o "$vb_dir/VisibilityBuffer.spv" 2>/dev/null; then
        sz=$(stat -f %z "$vb_dir/VisibilityBuffer.spv")
        printf "OK    Nanite/VisibilityBuffer.spv         (%d bytes)\n" "$sz"
    else echo "FAIL  Nanite/VisibilityBuffer (vert+frag link)"; fi
    rm -rf "$tmpdir"
else echo "MISS  Nanite/VisibilityBuffer.vert/.frag (skipped)"; fi

# VisibilityBufferResolve (compute, entry ComputeMain)
src="$DEST_DIR/Nanite/VisibilityBufferResolve.comp"
if [ -f "$src" ]; then
    out="$DEST_DIR/Nanite/VisibilityBufferResolve.spv"
    if glslangValidator -V --source-entrypoint main -e ComputeMain "$src" -o "$out" 2>/dev/null; then
        sz=$(stat -f %z "$out"); printf "OK    Nanite/VisibilityBufferResolve.spv (%d bytes)\n" "$sz"
    else echo "FAIL  Nanite/VisibilityBufferResolve.comp"; fi
else echo "MISS  Nanite/VisibilityBufferResolve.comp (skipped)"; fi

echo
echo "Done."
