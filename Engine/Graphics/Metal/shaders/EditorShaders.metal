// EditorShaders.metal — editor-only entry points compiled into the
// engine shaders metallib. The legacy engine shader loader in
// Engine/Graphics/Metal/MetalShader.cpp::load_engine_shaders looks
// up exactly 9 function names from the metallib (in this order):
//
//   0  fullscreen_triangle_vs
//   1  post_process_ps
//   2  shadow_mapping_vs
//   3  ssao_pass
//   4  ssao_blur
//   5  ssgi_pass
//   6  ssgi_blur
//   7  taa_pass
//   8  compose_pass
//
// We provide all 9 here, plus the editor's sphere shader pair which
// the legacy add_render_item path also needs (consumed via
// content::add_shader_group, but we keep the symbols together for
// build convenience). The 4 "real" implementations (fullscreen
// triangle, post-process PS, shadow mapping VS, plus the kernel
// stubs for ssao/ssgi) are written just well enough for the engine
// passes to construct pipelines and run end-to-end without crashing
// the driver. The Avalonia editor never enables TAA or compose, so
// those are pass-through fragments.
//
// This file is self-contained: it does NOT include the engine's
// Common.h / CommonTypes.metal / CommonConstants.metal /
// CommonFunction.metal. Those headers either pre-existing compiler
// errors (CommonConstants has a #error "include via Common.hlsli"
// that fires for xcrun metal -c, since __cplusplus is not defined
// in the .metal preprocessing) or define types the editor scene
// doesn't need. We define the small set of types and constants this
// file needs locally (see EditorGlobalData below). The actual
// rendering bindings still go through the engine's
// create_pso_if_needed which expects a GlobalData* in buffer 0 —
// our EditorGlobalData intentionally has the same [[id(N)]] field
// layout as the engine's GlobalData so the existing C++ binding
// code works without changes.

#include <metal_stdlib>
using namespace metal;

// Mirror of Engine/Graphics/Metal/shaders/CommonTypes.metal's
// GlobalData and GlobalShaderData. We only declare the fields the
// editor shaders actually read; create_pso_if_needed in the engine
// does not validate the GlobalData field set.
struct EditorGlobalShaderData
{
    float4x4 View;
    float4x4 Projection;
    float4x4 ViewProjection;
    float4x4 PreviousViewProjection;
    float4x4 InvViewProjection;
    float4  CameraPositionAndViewWidth;
    float4  CameraDirectionAndViewHeight;
    uint    NumDirectionalLights;
    float   DeltaTime;
    float   FrameCount;
    float4  _pad0;
    float4  _pad1;
    float4  _pad2;
    float4  _pad3;
};

struct EditorDirectionalLightParameters
{
    float4x4 LightMVP;
    float4   LightDirection;
    float4   LightColor;
    float4   LightParams;
};

struct EditorPerObjectData
{
    float4x4 World;
    float4x4 InvWorld;
    float4x4 WorldViewProjection;
    float4  sh_coeffs[9];
};

struct EditorGlobalData
{
    device const EditorGlobalShaderData* global_shader_data          [[id(0)]];
    device const EditorPerObjectData*    per_object_data             [[id(1)]];
    device const float3*                vertices                    [[id(2)]];
    device const void*                   elements                     [[id(3)]];
    device const uint*                   srv_indices                  [[id(4)]];
    device const EditorDirectionalLightParameters* directional_light_params [[id(5)]];
};

// 0. fullscreen_triangle_vs

struct FullscreenVSOut
{
    float4 position [[position]];
    float2 uv;
};

vertex FullscreenVSOut fullscreen_triangle_vs(uint vertexID [[vertex_id]])
{
    FullscreenVSOut o;
    o.uv = float2((float)((vertexID << 1) & 2), (float)(vertexID & 2) * 0.5f);
    o.position = float4(o.uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return o;
}

// 1. post_process_ps
//
// Minimal stub: returns a dark blue color (matching the engine's
// fallback clear color in MetalPostProcess.cpp line 197). The engine
// binds 4 textures + 1 sampler to this PSO at draw time, but the
// editor's surface is cleared to dark blue when post-process can't
// copy the G-buffer, so emitting that color directly is the
// visually-equivalent result. The Avalonia editor never enables
// post-process for a real scene; this stub exists only so the
// engine shader loader can resolve the function name and the fx
// pass can construct a pipeline state.

fragment float4 post_process_ps(FullscreenVSOut in [[stage_in]])
{
    return float4(0.05f, 0.05f, 0.08f, 1.0f);
}

// 2. shadow_mapping_vs

struct ShadowVSOut
{
    float4 HomogeneousPosition [[position]];
};

vertex ShadowVSOut shadow_mapping_vs(device const EditorGlobalData* global_data [[buffer(0)]],
                                     uint vertex_id [[vertex_id]])
{
    ShadowVSOut o;
    float4 pos = float4(global_data->vertices[vertex_id], 1.f);
    o.HomogeneousPosition = global_data->global_shader_data->ViewProjection * pos;
    return o;
}

// 3-6. ssao / ssgi compute kernel stubs

kernel void ssao_pass(texture2d<float, access::read>  in_tex  [[texture(0)]],
                      texture2d<float, access::write> out_tex [[texture(1)]],
                      uint2 gid [[thread_position_in_grid]])
{
    out_tex.write(in_tex.read(gid), gid);
}

kernel void ssao_blur(texture2d<float, access::read>  in_tex  [[texture(0)]],
                      texture2d<float, access::write> out_tex [[texture(1)]],
                      uint2 gid [[thread_position_in_grid]])
{
    out_tex.write(in_tex.read(gid), gid);
}

kernel void ssgi_pass(texture2d<float, access::read>  in_tex  [[texture(0)]],
                      texture2d<float, access::write> out_tex [[texture(1)]],
                      uint2 gid [[thread_position_in_grid]])
{
    out_tex.write(in_tex.read(gid), gid);
}

kernel void ssgi_blur(texture2d<float, access::read>  in_tex  [[texture(0)]],
                      texture2d<float, access::write> out_tex [[texture(1)]],
                      uint2 gid [[thread_position_in_grid]])
{
    out_tex.write(in_tex.read(gid), gid);
}

// 7-8. taa / compose fragment stubs

fragment float4 taa_pass(FullscreenVSOut in [[stage_in]])
{
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
}

fragment float4 compose_pass(FullscreenVSOut in [[stage_in]])
{
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
}

// Editor sphere shaders

struct SphereVSOut
{
    float4 HomogeneousPosition [[position]];
    float3 WorldPosition;
    float3 WorldNormal;
    float2 UV;
};

vertex SphereVSOut editor_sphere_vs(device const EditorGlobalData* global_data [[buffer(0)]],
                                   uint vertex_id [[vertex_id]])
{
    SphereVSOut vsOut;
    float4 pos = float4(global_data->vertices[vertex_id], 1.f);

    // The element buffer is 20B per vertex: float3 normal (12B) then
    // float2 uv (8B). The global_data struct types it as void*, so we
    // cast at the call site. Matches the byte layout that
    // EngineDLL/ResourceAPI.cpp::RegisterProceduralMeshSubmesh writes.
    struct RawElement
    {
        float3 normal;
        float2 uv;
    };
    device const RawElement* elements = (device const RawElement*)global_data->elements;
    RawElement el = elements[vertex_id];

    // The engine fills per_object_data.World with each entity's model
    // matrix (see MetalGPass::fill_per_object_data). Transform the
    // local-space vertex into world space first, then apply the engine's
    // view-projection. This way each entity's position drives where its
    // sphere appears on screen.
    float4 worldPos = global_data->per_object_data->World * pos;
    vsOut.HomogeneousPosition = global_data->global_shader_data->ViewProjection * worldPos;
    vsOut.WorldPosition = worldPos.xyz;
    float3x3 worldRot = float3x3(global_data->per_object_data->World.columns[0].xyz,
                                  global_data->per_object_data->World.columns[1].xyz,
                                  global_data->per_object_data->World.columns[2].xyz);
    vsOut.WorldNormal = normalize(worldRot * el.normal);
    vsOut.UV = el.uv;
    return vsOut;
}

fragment float4 editor_sphere_ps(SphereVSOut in [[stage_in]])
{
    // DBG: encode world position into RGB so we can see if per-entity
    // World matrix is correctly applied. -10..+10 maps to 0..1.
    float r = clamp((in.WorldPosition.x + 10.0) / 20.0, 0.0, 1.0);
    float g = clamp((in.WorldPosition.y + 10.0) / 20.0, 0.0, 1.0);
    float b = clamp((in.WorldPosition.z + 10.0) / 20.0, 0.0, 1.0);
    return float4(r, g, b, 1.0);
}
