# Engine/Graphics/Vulkan/shaders/

Production SPIR-V shaders for the Vulkan RHI backend.

## Source

Shaders here are ports of:
- `Engine/Graphics/Dawn/shaders/*.wgsl` (canonical WGSL source)
- `Engine/Graphics/Metal/shaders/*.metal` (legacy Metal source)

Compiled via:
- `naga` (WGSL → SPIR-V): `naga <name>.wgsl -o <name>.spv`
- `glslangValidator` (GLSL → SPIR-V): `glslangValidator -V -o <name>.spv <name>.vert/frag/comp`

Both `.spv` bytes and source files (.wgsl, .vert, .frag, .comp) are committed.

## Why both source and binary

Tint/naga compilation is deterministic but toolchain-version-sensitive. Locking in `.spv` bytes prevents drift between dev machines. Source is kept so recompilation is possible when shaders change.

See `build_spv.sh` for the canonical compile commands.

## Layout

- Root: PBR / shadow / depth / tone map / bloom / blur / SSR / IBL precompute
- `Nanite/` (future): GPU-driven meshlet pipeline
- `Lumen/` (future): GI passes

## Legacy files

The pre-existing `.glsl`, `.vert`, `.frag`, `.comp`, `.bat` files in this directory are from the dormant legacy `Engine/Graphics/Vulkan/` code (not in active CMake build). They are kept for reference but unused. New shaders use Tier 3 validated `.spv` files.
