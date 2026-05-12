#include <metal_stdlib>
using namespace metal;

// Interleaved vertex format: 32 bytes per vertex
// Matches SceneDataAdapter interleaved layout:
//   [0-11]  float3  position
//   [12-14] u8[3]   color
//   [15]    u8      t_sign
//   [16-19] u16[2]  packed normal
//   [20-23] u16[2]  packed tangent
//   [24-31] float2  uv
struct PackedVertex {
    float3  position;   // offset 0
    uint32_t packed_ct; // offset 12: color[3] + t_sign
    uint32_t packed_n;  // offset 16: packed normal u16[2]
    uint32_t packed_t;  // offset 20: packed tangent u16[2]
    float2  uv;         // offset 24
};

// Per-draw constant data (updated between draw calls)
struct CapturePassData {
    float4x4 view_proj;     // Card's orthographic VP matrix
    float4x4 world_matrix;  // Instance world transform
    float4   card_center;   // xyz = card center, w unused
    float4   card_extent;   // xyz = half extents, w unused
    uint     axis_direction; // low byte=axis, high byte=direction
    uint     material_id;   // Index into material data array
    uint     _pad[2];
};

// Material data (matches GPUMaterialRegistry::MaterialData)
struct MaterialData {
    uint32_t albedo_texture_idx;
    uint32_t normal_texture_idx;
    uint32_t orm_texture_idx;
    float    albedo_tint[3];
    float    metallic_factor;
    float    roughness_factor;
    float    normal_scale;
    float    uv_scale[2];
    uint32_t flags;
};

struct VertexOut {
    float4 position [[position]];
    float3 world_pos;
    float3 world_normal;
    float2 uv;
    uint   material_id;
    float  local_depth; // Depth from card plane
};

// Decode packed u16[2] normal to float3
static float3 decodePackedNormal(uint32_t packed) {
    float nx = float((packed >>  0) & 0xFFFF) / 65535.0 * 2.0 - 1.0;
    float ny = float((packed >> 16) & 0xFFFF) / 65535.0 * 2.0 - 1.0;
    float nz = sqrt(max(1.0 - nx*nx - ny*ny, 0.0));
    return float3(nx, ny, nz);
}

vertex VertexOut cardCaptureVS(
    uint vid [[vertex_id]],
    constant CapturePassData& pass [[buffer(0)]],
    constant PackedVertex* vertices [[buffer(1)]])
{
    PackedVertex v = vertices[vid];

    float4 worldPos = pass.world_matrix * float4(v.position, 1.0);
    float4 clipPos = pass.view_proj * worldPos;

    // Decode local-space normal and transform to world space
    float3 localNormal = decodePackedNormal(v.packed_n);
    // Simple normal matrix: upper-left 3x3 of world matrix (no non-uniform scale expected)
    float3 worldNormal = normalize((pass.world_matrix * float4(localNormal, 0.0)).xyz);

    // Compute depth from card plane
    float3 localToCard = worldPos.xyz - pass.card_center.xyz;
    uint axis = pass.axis_direction & 0xFF;
    float cardDepth = 0.0;
    if (axis == 0)      cardDepth = abs(localToCard.x);
    else if (axis == 1) cardDepth = abs(localToCard.y);
    else                cardDepth = abs(localToCard.z);

    VertexOut out;
    out.position = clipPos;
    out.world_pos = worldPos.xyz;
    out.world_normal = worldNormal;
    out.uv = v.uv;
    out.material_id = pass.material_id;
    out.local_depth = cardDepth;
    return out;
}

// Octahedral normal encoding
static float2 octEncode(float3 n) {
    float l1norm = abs(n.x) + abs(n.y) + abs(n.z);
    float2 result = n.xy / l1norm;
    if (n.z < 0.0) {
        result = (1.0 - abs(result.yx)) * select(float2(-1.0), float2(1.0), result.xy >= 0.0);
    }
    return result * 0.5 + 0.5;
}

struct FragmentOut {
    float4 albedo  [[color(0)]];
    float4 normal  [[color(1)]];
    float4 depth   [[color(2)]];
    float4 emissive [[color(3)]];
};

fragment FragmentOut cardCaptureFS(
    VertexOut in [[stage_in]],
    texture2d_array<float, access::sample> albedoArray [[texture(0)]],
    texture2d_array<float, access::sample> normalArray [[texture(1)]],
    constant MaterialData* materials [[buffer(3)]],
    sampler linearSampler [[sampler(0)]])
{
    MaterialData mat = materials[in.material_id];
    float2 uv = in.uv * float2(mat.uv_scale[0], mat.uv_scale[1]);

    float4 albedo = albedoArray.sample(linearSampler, uv, mat.albedo_texture_idx);
    float4 normalTex = normalArray.sample(linearSampler, uv, mat.normal_texture_idx);

    // Apply albedo tint
    float3 albedoColor = albedo.rgb * float3(mat.albedo_tint[0], mat.albedo_tint[1], mat.albedo_tint[2]);

    FragmentOut out;
    out.albedo = float4(albedoColor, albedo.a);
    out.normal = float4(octEncode(in.world_normal), 0.0, 1.0);
    out.depth = float4(in.local_depth, 0.0, 0.0, 1.0);
    out.emissive = float4(0.0, 0.0, 0.0, 1.0);
    return out;
}
