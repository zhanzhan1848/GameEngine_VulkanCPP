# Forward/ Vulkan SPIR-V Shaders (T4.6.5)

Hand-ported GLSL counterparts to `Engine/Graphics/Metal/shaders/Forward/*.metal`,
compiled to SPIR-V via `glslangValidator` and consumed by
`ForwardSceneRenderer::CreateShaders()` on Vulkan.

## Status (T4.6.5 part 10)

| Shader               | Stages       | Status   | Notes                                     |
|----------------------|--------------|----------|-------------------------------------------|
| DepthOnly            | vert         | PIPELINE-OK | T4.6.5 parts 2+13. Push constant 80B + InstanceBuffer SSBO binding 2 + vertex input declared. Pipeline creates successfully on Vulkan. |
| Skybox               | vert + frag  | DONE     | Procedural cube, samplerless tex          |
| Blit                 | vert + frag  | DONE     | T4.6.5 part 3 (Path A: tone-map ACES blit)|
| GBuffer              | vert + frag  | PIPELINE-OK | T4.6.5 parts 10-12. Loads + compiles + pipeline creates successfully on Vulkan. Runtime render not yet exercised (skip in Initialize() still active pending 7 remaining shader ports). |
| GBufferAlphaClip     | vert + frag  | TODO     | Alpha-tested foliage gate                 |
| GBufferUnlit         | vert + frag  | TODO     | Unlit emission                            |
| GBufferFoliage       | vert + frag  | TODO     | 2-pass foliage (alpha + lit)              |
| GBufferWater         | vert + frag  | TODO     | Animated water surface                    |
| GBufferTransparent   | vert + frag  | TODO     | Transparent GBuffer                       |
| ForwardTransparency  | vert + frag  | TODO     | 4 entries (Water + Transparent)           |
| StreamingGBuffer     | vert + frag  | TODO     | SoA vertex pulling                        |
| DeferredLighting     | (compute)    | Path B   | See "DeferredLighting path" below         |

11 shaders needed; 3 done; 1 partial (GBuffer loads, pipeline create blocked); 7 remaining + DeferredLighting path decision.

## T4.6.5 part 13 status

DepthOnly.vert (shadow pipeline) UNBLOCKED on Vulkan. Three fixes shipped:
1. **Push constant** — same `vec4 _use_pad` trick as GBuffer.vert part 11. PushConsts
   block now 80B (was 92B).
2. **InstanceBuffer SSBO** — added StorageBuffer binding 2 to global_set_layout_
   on Vulkan only. Metal path unchanged (uses `[[buffer(N)]]` auto-binding).
3. **Vertex input** — shadow pipeline gets single binding stride=32, 1 attribute
   (RGB32_Float position @0). Shader only reads `in_position`.

Verified by temporarily lifting skip + worktree-path hardcoded shader dir:
`shadow_pipeline_` and `gbuffer_pipeline_` both return OK handle. Zero
push-constant validation errors (was [0,92] before).

**Important verification gotcha (parts 11-13)**: hardcoded shader dir at
`ForwardSceneRenderer.cpp:127` points to main repo
(`GameEngine_VulkanCPP/Engine/Graphics/Vulkan/shaders/Forward/`), NOT the
worktree. macOS APFS is case-insensitive so "shaders" matches the main repo's
"Shaders" directory. For meaningful verification of worktree .spv changes,
temporarily edit the path to include `.worktrees/vulkan-rhi/`. The main repo
has STALE .spv files (no parts 11-13 fixes), so validation errors persist when
using the default path — but MoltenVK is lenient about push constant range
mismatches so pipelines still create. This is a pre-existing dev-machine-specific
hardcode that should be fixed in a future cleanup.

## T4.6.5 part 12 status

GBuffer pipeline NOW CREATES on Vulkan. Added explicit vertex input declaration
to `ForwardSceneRenderer.cpp` GBuffer pipeline create block: single binding
stride=32, 5 attributes mirroring ForwardRenderer bypassProd pattern but using
RG16_UInt for normal/tangent (matches GBuffer.vert's `uvec2` declaration).

Verified via temporarily lifting skip: `gbuffer_pipeline_` returns OK handle.
Remaining validation errors are from OTHER pipelines (shadow/DepthOnly), not
GBuffer.

## T4.6.5 part 11 status

Three of part 10's four blockers fixed:
1. ~~RG16_UInt missing in VulkanMath.h~~ — fixed at `VulkanMath.h:59`
2. ~~Push constant std140 layout~~ — fixed via `vec4 _use_pad` trick in GBuffer.vert
3. ~~DeferredLighting entry point mismatch~~ — load lambda in `ForwardSceneRenderer.cpp:635` now special-cases DeferredLighting compute to use `deferred_lighting_cs` instead of forcing `main`

Remaining blockers (NOT addressed in part 11+12+13):
1. ~~Vertex input declaration missing in C++ (GBuffer pipeline)~~ — fixed in part 12
2. ~~DepthOnly.vert push constant + InstanceBuffer + vertex input~~ — fixed in part 13
3. MoltenVK portability on RGB32_Float vertex format — turns out to be supported (no validation error); README claim was overstated
4. 7 of 11 ForwardSceneRenderer shaders still missing SPIR-V ports (alphaclip/unlit/foliage/water/transparent/ForwardTransparency/StreamingGBuffer)
5. Other pipelines (alphaclip/unlit/etc.) still need vertex input declaration once shaders are ported
6. Runtime instance buffer wiring — StorageBuffer binding 2 declared but no buffer attached yet (DepthOnly.vert references `instanceData.models[gl_InstanceIndex]` when use_instances != 0)

## Build

```bash
./build_spv.sh
```

Requires `glslangValidator` (`brew install glslang` or `/usr/local/bin/`).

## Naming convention

- `<Name>.vert` / `<Name>.frag` — GLSL source
- `<Name>.vert.spv` / `<Name>.frag.spv` — compiled SPIR-V (one entry point per file, named `main`)

The C++ loader `LoadShaderSource(filename, platform, stage)` dispatches on
stage to pick the suffix. `CreateShader` is then called with `"main"` as the
entry name on Vulkan (Metal still uses named entries like `vertexMain`).

## Descriptor set layout mapping

`ForwardSceneRenderer::CreateDescriptorLayouts` (cpp:505-559) declares:

| Set    | Purpose         | Bindings                                        |
|--------|-----------------|-------------------------------------------------|
| 0      | Global          | 0=ViewData UBO, 1=SceneData UBO                 |
| 1      | Material        | 0-2=SampledImage (albedo/normal/ORM), 3=Sampler |
| 2      | Lighting        | 0-1=UBO, 2-10=SampledImage, 11-12=Sampler       |
| 3      | Skybox          | 0=ViewData UBO, 1=SceneData UBO, 2-3=tex+samp   |
| 4      | Blit            | 0=SampledImage                                  |

Plus push constants (PCGPushConsts, offset 0): `mat4 transform; uint use_instances; uvec3 _pad;`

GLSL shaders must use `#extension GL_EXT_samplerless_texture_functions : enable`
and `texture(sampler2D(tex, samp), uv)` idiom — the engine uses separate
SampledImage + Sampler descriptors (not combined).

## Vertex input convention

Metal `[[buffer(N)]]` for vertex data maps to Vulkan vertex-buffer bindings
(NOT descriptor bindings). The 32-byte VertexInput stride in DepthOnly.metal
(packed_float3 position + 20 bytes padding) becomes a Vulkan vertex binding
with stride=32, position attribute at location 0 offset 0.

## Per-shader port checklist

For each remaining shader, the port workflow is:

1. Read the `.metal` source for entry-point signatures and buffer/texture bindings
2. Map `[[buffer(N)]]` for vertex input → Vulkan vertex buffer binding (descriptor at set 0/1/2)
3. Map `[[buffer(N)]]` for UBOs → set 0 binding (ViewData=0, SceneData=1)
4. Map `[[texture(N)]]` + `[[sampler(M)]]` → set N binding N, set N binding M (samplerless)
5. Map `[[push_constant]]` → `layout(push_constant) uniform PCGPushConsts`
6. Author GLSL `#version 450 core`
7. Add extension `GL_EXT_samplerless_texture_functions` if any textures
8. Run `./build_spv.sh`
9. Test by re-enabling ForwardSceneRenderer on Vulkan temporarily

## DeferredLighting path

`DeferredLighting.metal` has 11 entry points (vertexMain + 7 fragments + 2 compute
kernels). ForwardSceneRenderer consumes only 3: vertexMain (procedural full-screen
tri), fragmentLighting_v3 (PBR + IBL), fragmentBlit (tone-map).

The existing `Engine/Graphics/Vulkan/shaders/DeferredLighting.spv` (outside this
directory) is a single-entry **GLCompute** shader (`deferred_lighting_cs` → main),
compiled from `Dawn/shaders/DeferredLighting.wgsl`. Inputs/outputs use the engine
UBO layout (GlobalShaderData + ForwardLightBuffer), not Metal's ViewData/SceneData.

**T4.6.5 part 3 decision: Hybrid (Path B lighting + Path A blit).**

- **Blit (Path A, DONE)**: `Blit.vert` + `Blit.frag` in this directory author
  the tone-map blit as a graphics pipeline. Matches Metal fragmentBlit semantics
  (ACES + exposure + gamma). Replaces the multi-entry DeferredLighting.metal
  fragmentBlit path on Vulkan.
- **Lighting (Path B, TODO)**: Convert ForwardSceneRenderer's `lighting_pipeline_`
  from graphics to compute. Reuses existing `DeferredLighting.spv`. Requires:
  1. Add `TextureUsage::Storage` to `lighting_output_` (cpp:934).
  2. New compute-stage descriptor set layout matching the .spv's 12 bindings.
  3. Replace `CreateGraphicsPipeline` call (cpp:678) with `CreateComputePipeline`.
  4. Replace bind site (cpp:1754-1762) `BeginRenderPass+Draw` with
     `BindComputePipeline+Dispatch((W+7)/8, (H+7)/8, 1)`.

Binding layout for the .spv (set 0):
| Binding | Type                | Purpose                          |
|---------|---------------------|----------------------------------|
| 0-3     | SampledImage        | GBuffer albedo/normal/orm/velocity |
| 4       | SampledImage        | Shadow depth (2D array)          |
| 5-7     | SampledImage        | IBL irradiance/prefilter/brdfLUT |
| 8       | Sampler             | IBL sampler                      |
| 9       | UniformBuffer       | GlobalShaderData                 |
| 10      | UniformBuffer       | ForwardLightBuffer (25808B CSM)  |
| 11      | StorageImage        | HDR output                       |

Until Path B lands, ForwardSceneRenderer stays skipped on Vulkan (T4.6.3 skip
in `ForwardSceneRenderer.cpp:Initialize`).
