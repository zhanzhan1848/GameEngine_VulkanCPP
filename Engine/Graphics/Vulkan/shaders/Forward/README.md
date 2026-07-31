# Forward/ Vulkan SPIR-V Shaders (T4.6.5)

Hand-ported GLSL counterparts to `Engine/Graphics/Metal/shaders/Forward/*.metal`,
compiled to SPIR-V via `glslangValidator` and consumed by
`ForwardSceneRenderer::CreateShaders()` on Vulkan.

## Status (T4.6.5 part 2)

| Shader               | Stages       | Status   | Notes                              |
|----------------------|--------------|----------|------------------------------------|
| DepthOnly            | vert         | DONE     | Vertex-only (shadow depth pass)    |
| Skybox               | vert + frag  | DONE     | Procedural cube, samplerless tex   |
| GBuffer              | vert + frag  | TODO     | PBR GBuffer fill (complex)         |
| GBufferAlphaClip     | vert + frag  | TODO     | Alpha-tested foliage gate          |
| GBufferUnlit         | vert + frag  | TODO     | Unlit emission                     |
| GBufferFoliage       | vert + frag  | TODO     | 2-pass foliage (alpha + lit)       |
| GBufferWater         | vert + frag  | TODO     | Animated water surface             |
| GBufferTransparent   | vert + frag  | TODO     | Transparent GBuffer                |
| ForwardTransparency  | vert + frag  | TODO     | 4 entries (Water + Transparent)    |
| StreamingGBuffer     | vert + frag  | TODO     | SoA vertex pulling                 |
| DeferredLighting     | vert + frag  | BLOCKER  | Architectural mismatch (see below) |

11 shaders needed; 2 done; 9 remaining.

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

## DeferredLighting architectural blocker

`DeferredLighting.metal` has 9 entry points spanning vertex + 7 fragment stages
(vertexMain, fragmentLighting_v3, fragmentBlit, fragmentBlitDDGI,
fragmentLighting_gpuDriven, fragmentBlitComposite, fragmentFusionIndirect,
fragmentFusion). The existing `Engine/Graphics/Vulkan/shaders/DeferredLighting.spv`
(outside this directory) was compiled from `Dawn/shaders/DeferredLighting.wgsl`
as a **GLCompute** shader (`deferred_lighting_cs`) — fundamentally different
rendering architecture.

Two paths forward (multi-session scope):
1. **Match Metal** — author 9 SPIR-V vert/frag shaders matching the Metal path's
   rasterization-based deferred lighting. Required if ForwardSceneRenderer keeps
   its current shape.
2. **Match Dawn** — rewrite ForwardSceneRenderer to dispatch deferred lighting
   as compute. Bigger code change but reuses existing compute shader.

Until this is resolved, ForwardSceneRenderer stays skipped on Vulkan (T4.6.3
skip in `ForwardSceneRenderer.cpp:Initialize`).
