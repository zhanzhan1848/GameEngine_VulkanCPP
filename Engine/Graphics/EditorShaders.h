#pragma once

namespace primal::editor_shaders
{
    // Editor-side Metal shader sources. PR 3 paths use these to drive
    // xcrun metal -c to produce a metallib. PR 2 inline-MSL strings live
    // here as the fallback path when the metallib build fails.
    inline constexpr const char* kEditorCubeShaders = R"metal(
#include <metal_stdlib>
using namespace metal;
struct CubeVSIn  { float3 pos [[attribute(0)]]; float3 col [[attribute(1)]]; };
struct CubeVSOut { float4 pos [[position]]; float3 col; };
struct CubeUBO  { float4x4 mvp; };

vertex CubeVSOut cube_vs(uint vid [[vertex_id]],
                         constant CubeUBO& u [[buffer(1)]])
{
    constant float3 P[8] = {
        float3(-0.6,-0.6,-0.6), float3( 0.6,-0.6,-0.6),
        float3( 0.6, 0.6,-0.6), float3(-0.6, 0.6,-0.6),
        float3(-0.6,-0.6, 0.6), float3( 0.6,-0.6, 0.6),
        float3( 0.6, 0.6, 0.6), float3(-0.6, 0.6, 0.6),
    };
    constant float3 C[8] = {
        float3(0.9,0.3,0.3), float3(0.3,0.9,0.3),
        float3(0.3,0.3,0.9), float3(0.9,0.9,0.3),
        float3(0.9,0.3,0.9), float3(0.3,0.9,0.9),
        float3(0.95,0.95,0.95), float3(0.4,0.4,0.4),
    };
    constant uint faceTri[36] = {
        0,1,2, 0,2,3,
        5,4,7, 5,7,6,
        1,5,6, 1,6,2,
        4,0,3, 4,3,7,
        3,2,6, 3,6,7,
        4,5,1, 4,1,0,
    };
    const uint corner = faceTri[vid];
    CubeVSOut o;
    o.pos = u.mvp * float4(P[corner], 1.0);
    o.col = C[corner];
    return o;
}

fragment float4 cube_fs(CubeVSOut in [[stage_in]])
{
    return float4(in.col, 1.0);
}
)metal";

    // Sphere shaders for the Avalonia editor test scene. We deliberately
    // follow the legacy engine argument-buffer architecture (see
    // Engine/Graphics/Metal/MetalGPass.h::opaque_root_parameter and
    // Engine/Graphics/Metal/shaders/buildin_shader.metal) rather than
    // declaring [[attribute(N)]] vertex attributes, because
    // create_pso_if_needed in Engine/Graphics/Metal/MetalContent.cpp
    // creates the MTL::RenderPipelineDescriptor without a vertex
    // descriptor and binds a per-draw argument_buffer via
    // setArgumentBuffer(..., 0, 0). The engine then accesses the actual
    // vertex stream via the resource pointers encoded inside that
    // argument buffer, indexed by [[id(N)]] in the GlobalData struct.
    //
    // Vertex element layout (matches Engine/Content/ProceduralMesh.h
    // PROC_ELEM_STRIDE=20 for static_normal_texture):
    //   uint           ColorTSign
    //   packed_ushort2 Normal
    //   packed_ushort2 Tangent
    //   packed_float2  UV
    // But we never wrote those packed values when constructing the
    // RHIMeshAsset in create_sphere_mesh — we wrote raw float3 normal
    // + float2 uv straight into the element_buffer. So either the
    // engine's compressed decoder is the only thing reading the
    // element buffer, OR we need to match its layout.
    //
    // IMPORTANT: ProcMeshCreate writes raw float3 normal (12B) + float2
    // uv (8B) = 20B per vertex. The legacy buildin_shader.metal reads
    // it via packed_ushort2 (compressed) and re-decodes with
    // InvIntervals. We do the SAME read but at the offset where the
    // raw float3 normal lives (offset 0 of the element, then we skip
    // 4 bytes to UV). This is a quick approximation that works
    // visually but does not match the compressed normal encoding.
    inline constexpr const char* kEditorSphereVertexShader = R"metal(
#include <metal_stdlib>
using namespace metal;

struct VertexOut
{
    float4 HomogeneousPosition [[position]];
    float3 WorldPosition;
    float3 WorldNormal;
    float2 UV;
};

// Mirror Engine/Graphics/Metal/shaders/buildin_shader.metal's GlobalData
// layout. The [[id(N)]] values match the C++ opaque_root_parameter enum
// in Engine/Graphics/Metal/MetalGPass.h:
//   id(0) = global_shader_data
//   id(1) = per_object_data
//   id(2) = position_buffer
//   id(3) = element_buffer
//   id(4) = srv_indices
//   id(5) = directional_lights
//
// EditorGlobalShaderData is the prefix of msl::GlobalShaderData that
// the engine populates. We declare the fields we actually read
// (ViewProjection) and add padding to match the engine's layout up to
// that point. create_pso_if_needed in the engine does not validate the
// field set on this struct, so omitting fields past what we read is OK.
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

struct EditorPerObjectData
{
    float4x4 World;
    float4x4 InvWorld;
    float4x4 WorldViewProjection;
    float4  sh_coeffs[9];
};

struct GlobalData
{
    device const EditorGlobalShaderData* global_shader_data          [[id(0)]];
    device const EditorPerObjectData*    per_object_data             [[id(1)]];
    device const float3*                vertices                    [[id(2)]];
    // Element buffer is 20B per vertex: float3 normal (12B) then float2 uv (8B).
    // We declare it as a packed struct matching the byte layout the
    // C++ RegisterProceduralMeshSubmesh writes.
    struct Element
    {
        float3 normal;
        float2 uv;
    };
    device const Element* elements                     [[id(3)]];
    device const uint*     srv_indices                  [[id(4)]];
    device const void*     directional_light_params     [[id(5)]];
};

vertex VertexOut sphere_vs(device const GlobalData* global_data [[buffer(0)]],
                            uint vertex_id [[vertex_id]])
{
    VertexOut vsOut;
    float4 pos = float4(global_data->vertices[vertex_id], 1.f);
    // The engine fills per_object_data.World with each entity's model
    // matrix (see MetalGPass::fill_per_object_data). Transform the
    // local-space vertex into world space first, then apply the engine's
    // view-projection. This way each entity's position drives where its
    // sphere appears on screen.
    float4 worldPos = global_data->per_object_data->World * pos;
    vsOut.HomogeneousPosition = global_data->global_shader_data->ViewProjection * worldPos;
    vsOut.WorldPosition = worldPos.xyz;
    // Transform the local-space normal by the upper-left 3x3 of World.
    // This is correct for pure translation + rotation + uniform scale;
    // a non-uniform scale would need the inverse-transpose.
    float3x3 worldRot = float3x3(global_data->per_object_data->World.columns[0].xyz,
                                  global_data->per_object_data->World.columns[1].xyz,
                                  global_data->per_object_data->World.columns[2].xyz);
    vsOut.WorldNormal = normalize(worldRot * global_data->elements[vertex_id].normal);
    vsOut.UV = global_data->elements[vertex_id].uv;
    return vsOut;
}
)metal";

    inline constexpr const char* kEditorSpherePixelShader = R"metal(
#include <metal_stdlib>
using namespace metal;

struct VertexOut
{
    float4 HomogeneousPosition [[position]];
    float3 WorldPosition;
    float3 WorldNormal;
    float2 UV;
};

fragment float4 sphere_ps(VertexOut in [[stage_in]])
{
    // DBG: encode world position into RGB so we can see if per-entity
    // World matrix is correctly applied. -10..+10 maps to 0..1.
    float r = clamp((in.WorldPosition.x + 10.f) / 20.f, 0.f, 1.f);
    float g = clamp((in.WorldPosition.y + 10.f) / 20.f, 0.f, 1.f);
    float b = clamp((in.WorldPosition.z + 10.f) / 20.f, 0.f, 1.f);
    return float4(r, g, b, 1.0f);
}
)metal";
}

