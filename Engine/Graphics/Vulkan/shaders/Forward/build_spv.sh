#!/bin/bash
# build_spv.sh — compile Forward/ GLSL shaders to SPIR-V for Vulkan.
#
# T4.6.5 part 2: Each Metal .metal shader in Engine/Graphics/Metal/shaders/Forward/
# gets a hand-ported GLSL counterpart here. glslangValidator compiles each .vert/.frag
# to a stage-suffixed .spv file consumed by ForwardSceneRenderer.
#
# Naming convention: <Name>.vert.spv and <Name>.frag.spv with entry point "main".
# C++ LoadShaderSource dispatches on ShaderStage to pick the suffix.
#
# Requirements:
#   - glslangValidator (/usr/local/bin or brew)
#
# Re-run after touching any source:
#     ./build_spv.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

GLSLANG=${GLSLANG:-glslangValidator}

# Shader name → "vert", "frag", or "both"
# Maps to ForwardSceneRenderer::CreateShaders() load() calls (cpp:536-568)
declare -a SHADERS=(
    # name                stages
    "DepthOnly            vert"
    "Skybox               vert frag"
    "Blit                 vert frag"   # T4.6.5 part 3: tone-map blit (replaces DeferredLighting fragmentBlit)
    "GBuffer              vert frag"
    "GBufferAlphaClip     vert frag"
    "GBufferUnlit         vert frag"
    "GBufferFoliage       vert frag"
    "GBufferWater         vert frag"
    "GBufferTransparent   vert frag"
    # T4.6.5 part 14: Metal ForwardTransparency.metal has 4 entry points; Vulkan
    # splits into one SPIR-V per entry point → ForwardWater.{vert,frag} and
    # ForwardTransparent.{vert,frag}. Each .spv has a single `main` entry.
    "ForwardWater         vert frag"
    "ForwardTransparent   vert frag"
    "StreamingGBuffer     vert frag"
    # DeferredLighting special: Path B will use existing compute .spv; see README
)

for entry in "${SHADERS[@]}"; do
    set -- $entry
    name=$1; shift
    for stage in "$@"; do
        src="${name}.${stage}"
        if [ -f "$src" ]; then
            out="${name}.${stage}.spv"
            echo "[compile] $src → $out"
            "$GLSLANG" -V100 -S "$stage" "$src" -o "$out"
        else
            echo "[skip]    $src not yet authored"
        fi
    done
done

echo
echo "Done. Generated .spv files:"
ls -1 *.spv 2>/dev/null || echo "  (none)"
