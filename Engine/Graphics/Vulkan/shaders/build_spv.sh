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
    SSGITrace                    # SSGI (Phase 3)
    SSGITemporal
    SSGIFilter
    SSGIHalfResDenoise
)

inline_includes() {
    local src="$1"
    local dir
    dir="$(dirname "$src")"
    # Recursive include inlining. Phase 0.6: GLSL headers may themselves
    # include other headers (RHIShaderCommon.glsl → RHIShaderConstants.glsl
    # + RHIShaderTypes.glsl + RHIShaderFunctions.glsl → RHIShaderPBR.glsl).
    # All headers live in the same dir, so include resolution uses a single
    # base directory. Include guards (#ifndef ... #endif) prevent duplicate
    # emission when a header is reached via multiple paths.
    awk -v file="$src" -v base="$dir" '
        function process(f, b,    line, inc, inc_path) {
            while ((getline line < f) > 0) {
                if (line ~ /^#include[[:space:]]*"[^"]+"/) {
                    match(line, /"[^"]+"/)
                    inc = substr(line, RSTART+1, RLENGTH-2)
                    inc_path = b "/" inc
                    process(inc_path, b)
                } else {
                    print line
                }
            }
            close(f)
        }
        BEGIN { process(file, base) }
    '
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
#
# Phase 0.6: GLSL sources may `#include "RHIShader*.glsl"` for shared structs /
# helpers. inline_includes (awk-based) inlines those headers before passing to
# glslangValidator. Headers themselves are not compiled standalone.
GLSL_NAMES=( "Particle/Particle" "Forward/Line" "Forward/DeferredLighting"
             "Debug/MeshletDebug" "Debug/SDFDebug" "Debug/VectorFieldDebug" "Debug/VoxelDebug" )
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
        tmpdir="$(mktemp -d)"
        tmp="$tmpdir/$src_name.$stage"
        inline_includes "$src" > "$tmp"
        if glslangValidator --quiet -V "$tmp" -o "$out" 2>"$tmpdir/err"; then
            sz=$(stat -f %z "$out")
            printf "OK    %-36s (%d bytes)\n" "$src_dir/$src_name.$stage.spv" "$sz"
        else
            echo "FAIL  $src_dir/$src_name.$stage"
            sed 's/^/    /' "$tmpdir/err"
        fi
        rm -rf "$tmpdir"
    done
done

# T4.6.5 part 17.2+: Nanite hand-written GLSL. Each is a single-stage compute
# or a combined vert+frag (VisibilityBuffer) that needs spirv-link to merge.
# Compute: simple glslangValidator call with --source-entrypoint main -e NAME.
# VisibilityBuffer: compile vert+frag separately, then spirv-link into one .spv.
# Phase 0.6: all Nanite GLSL also goes through inline_includes to support
# `#include "RHIShader*.glsl"` headers.
echo
echo "==> Nanite hand-written GLSL ===="

# ClusterBinning (compute, entry cluster_binning_kernel)
src="$DEST_DIR/Nanite/ClusterBinning.comp"
if [ -f "$src" ]; then
    out="$DEST_DIR/Nanite/ClusterBinning.spv"
    tmpdir="$(mktemp -d)"; tmp="$tmpdir/ClusterBinning.comp"
    inline_includes "$src" > "$tmp"
    if glslangValidator --quiet -V --source-entrypoint main -e cluster_binning_kernel "$tmp" -o "$out" 2>"$tmpdir/err"; then
        sz=$(stat -f %z "$out"); printf "OK    Nanite/ClusterBinning.spv          (%d bytes)\n" "$sz"
    else echo "FAIL  Nanite/ClusterBinning.comp"; sed 's/^/    /' "$tmpdir/err"; fi
    rm -rf "$tmpdir"
else echo "MISS  Nanite/ClusterBinning.comp (skipped)"; fi

# VisibilityBuffer (combined vert+frag, 2 entries)
vb_dir="$DEST_DIR/Nanite"
if [ -f "$vb_dir/VisibilityBuffer.vert" ] && [ -f "$vb_dir/VisibilityBuffer.frag" ]; then
    tmpdir="$(mktemp -d)"
    inline_includes "$vb_dir/VisibilityBuffer.vert" > "$tmpdir/vb.vert"
    inline_includes "$vb_dir/VisibilityBuffer.frag" > "$tmpdir/vb.frag"
    if glslangValidator --quiet -V --source-entrypoint main -e visibility_vertex_shader \
           "$tmpdir/vb.vert" -o "$tmpdir/vb.vert.spv" 2>"$tmpdir/err_v" && \
       glslangValidator --quiet -V --source-entrypoint main -e visibility_fragment_shader \
           "$tmpdir/vb.frag" -o "$tmpdir/vb.frag.spv" 2>"$tmpdir/err_f" && \
       spirv-link "$tmpdir/vb.vert.spv" "$tmpdir/vb.frag.spv" -o "$vb_dir/VisibilityBuffer.spv" 2>"$tmpdir/err_link"; then
        sz=$(stat -f %z "$vb_dir/VisibilityBuffer.spv")
        printf "OK    Nanite/VisibilityBuffer.spv         (%d bytes)\n" "$sz"
    else echo "FAIL  Nanite/VisibilityBuffer (vert+frag link)";
        [ -s "$tmpdir/err_v" ] && sed 's/^/    [v] /' "$tmpdir/err_v"
        [ -s "$tmpdir/err_f" ] && sed 's/^/    [f] /' "$tmpdir/err_f"
        [ -s "$tmpdir/err_link" ] && sed 's/^/    [link] /' "$tmpdir/err_link"
    fi
    rm -rf "$tmpdir"
else echo "MISS  Nanite/VisibilityBuffer.vert/.frag (skipped)"; fi

# VisibilityBufferResolve (compute, entry ComputeMain)
src="$DEST_DIR/Nanite/VisibilityBufferResolve.comp"
if [ -f "$src" ]; then
    out="$DEST_DIR/Nanite/VisibilityBufferResolve.spv"
    tmpdir="$(mktemp -d)"; tmp="$tmpdir/VisibilityBufferResolve.comp"
    inline_includes "$src" > "$tmp"
    if glslangValidator --quiet -V --source-entrypoint main -e ComputeMain "$tmp" -o "$out" 2>"$tmpdir/err"; then
        sz=$(stat -f %z "$out"); printf "OK    Nanite/VisibilityBufferResolve.spv (%d bytes)\n" "$sz"
    else echo "FAIL  Nanite/VisibilityBufferResolve.comp"; sed 's/^/    /' "$tmpdir/err"; fi
    rm -rf "$tmpdir"
else echo "MISS  Nanite/VisibilityBufferResolve.comp (skipped)"; fi

# T4.6.5 part 40: ShadowFilter (compute, entry main) — half-res visibility
# producer consumed by DeferredLighting binding 6. Mirrors
# EngineTest/shaders/ShadowFilter.metal:94-148.
src="$DEST_DIR/ShadowFilter.comp"
if [ -f "$src" ]; then
    out="$DEST_DIR/ShadowFilter.spv"
    tmpdir="$(mktemp -d)"; tmp="$tmpdir/ShadowFilter.comp"
    inline_includes "$src" > "$tmp"
    if glslangValidator --quiet -V --source-entrypoint main -e main "$tmp" -o "$out" 2>"$tmpdir/err"; then
        sz=$(stat -f %z "$out"); printf "OK    ShadowFilter.spv                       (%d bytes)\n" "$sz"
    else echo "FAIL  ShadowFilter.comp"; sed 's/^/    /' "$tmpdir/err"; fi
    rm -rf "$tmpdir"
else echo "MISS  ShadowFilter.comp (skipped)"; fi

# Toon cel-shading post process (compute, entry main) — color quantization +
# depth Sobel edges. Mirrors Metal/shaders/ToonShader.metal (entry toon_main).
src="$DEST_DIR/PostProcess/Toon.comp"
if [ -f "$src" ]; then
    out="$DEST_DIR/PostProcess/Toon.comp.spv"
    tmpdir="$(mktemp -d)"; tmp="$tmpdir/Toon.comp"
    inline_includes "$src" > "$tmp"
    if glslangValidator --quiet -V --source-entrypoint main -e main "$tmp" -o "$out" 2>"$tmpdir/err"; then
        sz=$(stat -f %z "$out"); printf "OK    PostProcess/Toon.comp.spv              (%d bytes)\n" "$sz"
    else echo "FAIL  PostProcess/Toon.comp"; sed 's/^/    /' "$tmpdir/err"; fi
    rm -rf "$tmpdir"
else echo "MISS  PostProcess/Toon.comp (skipped)"; fi

# VSM shadow moments fragment (entry main) — lone fragment; the vertex stage
# reuses ShadowDepth.spv's shadow_depth_vs. Mirrors
# EngineTest/shaders/ShadowMoments.metal (entry shadow_moments_fs).
src="$DEST_DIR/Nanite/ShadowMoments.frag"
if [ -f "$src" ]; then
    out="$DEST_DIR/Nanite/ShadowMoments.frag.spv"
    tmpdir="$(mktemp -d)"; tmp="$tmpdir/ShadowMoments.frag"
    inline_includes "$src" > "$tmp"
    if glslangValidator --quiet -V "$tmp" -o "$out" 2>"$tmpdir/err"; then
        sz=$(stat -f %z "$out"); printf "OK    Nanite/ShadowMoments.frag.spv         (%d bytes)\n" "$sz"
    else echo "FAIL  Nanite/ShadowMoments.frag"; sed 's/^/    /' "$tmpdir/err"; fi
    rm -rf "$tmpdir"
else echo "MISS  Nanite/ShadowMoments.frag (skipped)"; fi

# ============================================================================
# Lumen WGSL (naga) — Phase 2 DDGI shaders from Dawn/shaders/Lumen/.
# DDGI uses WGSL "Mode 11 canonical" path (self-circulating irradiance_history +
# gbuffer_albedo, not Metal's SurfaceCache-sampling). Decision D3-A in
# Docs/2026-08-09-vulkan-lumen-gi-port-design.md.
# Output to Vulkan/shaders/Lumen/<name>.spv (no .comp infix — WGSL single entry).
# ============================================================================
echo
echo "==> Lumen WGSL (naga) ===="
mkdir -p "$DEST_DIR/Lumen"

LUMEN_WGSL_SHADERS=(
    DDGITraceRays                # ray tracing via GlobalSDF + irradiance self-circulation
    DDGIUpdateIrradiance         # EMA blend ray results into probe SH irradiance
    DDGIUpdateDepth              # update octahedral probe depth (mean+variance)
    DDGIGIGather                 # DDGI probe irradiance -> half-res screen GI texture
)

for s in "${LUMEN_WGSL_SHADERS[@]}"; do
    src="$ENGINE_DAWN_DIR/Lumen/$s.wgsl"
    if [ ! -f "$src" ]; then
        printf "MISS  Lumen/%s.wgsl (skipped)\n" "$s"
        continue
    fi
    tmpdir="$(mktemp -d)"
    tmp="$tmpdir/$s.wgsl"
    inline_includes "$src" > "$tmp"
    if naga "$tmp" "$DEST_DIR/Lumen/$s.spv" 2>"$tmpdir/err"; then
        sz=$(stat -f %z "$DEST_DIR/Lumen/$s.spv")
        printf "OK    Lumen/%-26s (%d bytes)\n" "$s.spv" "$sz"
    else
        printf "FAIL  Lumen/%s.wgsl\n" "$s"
        sed 's/^/    /' "$tmpdir/err"
    fi
    rm -rf "$tmpdir"
done

# ============================================================================
# Lumen hand-written GLSL (Phase 1+ of Vulkan Lumen/GI port).
# SSAO/SSGI/GIGather/Fusion are hand-written GLSL ports of Metal .metal shaders
# (decisions D2-A / D7 in Docs/2026-08-09-vulkan-lumen-gi-port-design.md —
# hand-written GLSL replicating Metal algorithms for visual parity, rather than
# reusing the divergent Dawn WGSL hemisphere-sampling variants).
# DDGI uses the existing Dawn WGSL (compiled via LUMEN_WGSL_SHADERS above, to be
# added in Phase 2). This block only compiles the hand-written GLSL .comp files.
#
# Each entry: "<Subdir>/<Name> <entry_point>"
#   - source:   $DEST_DIR/<Subdir>/<Name>.comp
#   - output:   $DEST_DIR/<Subdir>/<Name>.comp.spv
#   - compile:  inline_includes | glslangValidator -V --source-entrypoint main -e <entry>
# ============================================================================
echo
echo "==> Lumen hand-written GLSL ===="
mkdir -p "$DEST_DIR/Lumen"

# Parallel arrays (Bash 3.2 on macOS lacks associative arrays).
# Compute shaders use .comp → .comp.spv; fragment shaders use .frag → .frag.spv.
LUMEN_GLSL_NAMES=( "Lumen/SSAOTrace" "Lumen/SSAOFilter" "Lumen/DDGIGIGather" "Lumen/FusionIndirect" "Lumen/FusionComposite" "Lumen/DDGITraceRays" "Lumen/DDGIUpdateIrradiance" "Lumen/DDGIUpdateDepth" "Lumen/SSGITrace" "Lumen/SSGITemporal" "Lumen/SSGIFilter" "Lumen/SSGIHalfResDenoise" "Lumen/SSRTrace" "Lumen/SSRTemporal" "Lumen/SSRComposite" "Lumen/TAA" "Lumen/SurfaceCacheDilate" "Lumen/SurfaceCacheLightCull" "Lumen/SurfaceCacheLightEval" "Lumen/SurfaceCacheIndirectTrace" "Lumen/SurfaceCacheIndirectResolve" "Lumen/SurfaceCacheAtlasInit" "Lumen/ScreenProbePlace" "Lumen/ScreenProbeTraceRays" "Lumen/ScreenProbeAverage" "Lumen/ScreenProbeTemporal" "Lumen/ScreenProbeSpatialFilter" "Lumen/ScreenProbeGather" "Lumen/ScreenProbeDenoise" "Lumen/DDGICardRadianceAvg" "Lumen/DDGIProbeIrradianceFromCards" "Lumen/SurfaceCacheCapture" "Lumen/DDGITraceSDF" "Lumen/DDGITraceFinalize" )
LUMEN_GLSL_ENTRIES=( "ssao_trace" "ssao_filter" "ddgi_gi_gather" "fusion_indirect" "fusion_composite" "ddgi_trace_rays" "ddgi_update_irradiance" "ddgi_update_depth" "ssgi_trace" "ssgi_temporal" "ssgi_filter" "ssgi_halfres_denoise" "ssr_trace" "ssr_temporal" "ssr_composite" "taa_main" "surface_cache_dilate" "surface_cache_light_cull" "surface_cache_light_eval" "surface_cache_indirect_trace" "surface_cache_indirect_resolve" "surface_cache_atlas_init" "screen_probe_place" "screen_probe_trace_rays" "screen_probe_average" "screen_probe_temporal" "screen_probe_spatial_filter" "screen_probe_gather" "screen_probe_denoise" "ddgi_card_radiance_avg" "ddgi_probe_irradiance_from_cards" "surface_cache_capture" "ddgi_trace_sdf" "ddgi_trace_finalize" )
LUMEN_GLSL_STAGES=( "comp" "comp" "comp" "frag" "frag" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" "comp" )

for i in "${!LUMEN_GLSL_NAMES[@]}"; do
    pair="${LUMEN_GLSL_NAMES[$i]}"
    entry="${LUMEN_GLSL_ENTRIES[$i]}"
    stage="${LUMEN_GLSL_STAGES[$i]}"
    src_dir="$(dirname "$pair")"
    src_name="$(basename "$pair")"
    src="$DEST_DIR/$src_dir/$src_name.$stage"
    if [ ! -f "$src" ]; then
        printf "MISS  %s/%s.%s (skipped)\n" "$src_dir" "$src_name" "$stage"
        continue
    fi
    out="$DEST_DIR/$src_dir/$src_name.$stage.spv"
    tmpdir="$(mktemp -d)"; tmp="$tmpdir/$src_name.$stage"
    inline_includes "$src" > "$tmp"
    if glslangValidator --quiet -V --source-entrypoint main -e "$entry" "$tmp" -o "$out" 2>"$tmpdir/err"; then
        sz=$(stat -f %z "$out")
        printf "OK    %s/%-26s (%d bytes)\n" "$src_dir" "$src_name.$stage.spv" "$sz"
    else
        printf "FAIL  %s/%s.%s\n" "$src_dir" "$src_name" "$stage"
        sed 's/^/    /' "$tmpdir/err"
    fi
    rm -rf "$tmpdir"
done

# ============================================================================
# Debug visualization GLSL shaders.
# Hand-written GLSL fragment shaders for fullscreen debug views (e.g. SDF
# visualization).  All use entry point "main".
# Each entry: "<Subdir>/<Name>"
#   - source:   $DEST_DIR/<Subdir>/<Name>.frag
#   - output:   $DEST_DIR/<Subdir>/<Name>.frag.spv
# ============================================================================
echo
echo "==> Debug GLSL ===="
mkdir -p "$DEST_DIR/Debug"

DEBUG_GLSL_NAMES=( "Debug/SDFVisualization" )
DEBUG_GLSL_STAGES=( "frag" )

for i in "${!DEBUG_GLSL_NAMES[@]}"; do
    pair="${DEBUG_GLSL_NAMES[$i]}"
    stage="${DEBUG_GLSL_STAGES[$i]}"
    src_dir="$(dirname "$pair")"
    src_name="$(basename "$pair")"
    src="$DEST_DIR/$src_dir/$src_name.$stage"
    if [ ! -f "$src" ]; then
        printf "MISS  %s/%s.%s (skipped)\n" "$src_dir" "$src_name" "$stage"
        continue
    fi
    out="$DEST_DIR/$src_dir/$src_name.$stage.spv"
    tmpdir="$(mktemp -d)"; tmp="$tmpdir/$src_name.$stage"
    inline_includes "$src" > "$tmp"
    if glslangValidator --quiet -V "$tmp" -o "$out" 2>"$tmpdir/err"; then
        sz=$(stat -f %z "$out")
        printf "OK    %s/%-26s (%d bytes)\n" "$src_dir" "$src_name.$stage.spv" "$sz"
    else
        printf "FAIL  %s/%s.%s\n" "$src_dir" "$src_name" "$stage"
        sed 's/^/    /' "$tmpdir/err"
    fi
    rm -rf "$tmpdir"
done

echo
echo "Done."
