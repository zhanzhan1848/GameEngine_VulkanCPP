// MirrorReflection.metal — Metal counterpart of
// Vulkan/shaders/Nanite/MirrorReflection.vert/.frag (hand-written GLSL).
// Entry points: mirror_reflection_vs / mirror_reflection_fs.
// Renders the scene from a reflected camera via vertex pulling, simplified
// forward lambert + ambient, albedo from the material texture array.
#include <metal_stdlib>
using namespace metal;

// Flat slots match the C++ reflection descriptor layout (binding N → buffer(N)/texture(N)).
struct ReflectionParams {
    float4x4 reflected_view_projection;
    float4 light_dir;
    float4 light_color;
    float4 ambient;
};

struct Meshlet {
    uint4 header;   // vertex_offset, triangle_offset, vertex_count, triangle_count
    float4 cone0;
    float4 cone1;
    float4 cone2;
};

struct InstanceData {
    float4x4 world_matrix;
    float4x4 inverse_world_matrix;
    uint4 meta0;
    uint4 meta1;
    float4 bounds_center_pad;
    float4 bounds_radius_pad;
};

struct VSOut {
    float4 position [[position]];
    float2 uv [[user(loc0)]];
    float3 normalW [[user(loc1)]];
    float3 albedoTint [[user(loc2)]];
    uint albedoIdx [[user(loc3)]];
};

static float3 unpackNormal(uint packed, uint colorTSign) {
    float hi = float((packed >> 16u) & 0xFFFFu);
    float lo = float(packed & 0xFFFFu);
    float2 f = float2(hi, lo) * (2.0 / 65535.0) - float2(1.0);
    float d = dot(f, f);
    if (d > 1.0) return float3(0.0, 0.0, 1.0);
    float z = sqrt(max(0.0, 1.0 - d));
    uint signs = (colorTSign >> 24u) & 0xFFu;
    float nSign = float(signs & 0x02u) - 1.0;
    return float3(f.x, f.y, z * nSign);
}

vertex VSOut mirror_reflection_vs(
    uint vertexID [[vertex_id]],
    uint listIdx [[instance_id]],
    constant ReflectionParams& params [[buffer(0)]],
    constant Meshlet* meshlets [[buffer(1)]],
    constant uint* meshletVertices [[buffer(2)]],
    constant uint* meshletTrianglesPacked [[buffer(3)]],
    constant float* positions [[buffer(4)]],
    constant uint* elements [[buffer(5)]],
    constant uint4* reflVisible [[buffer(6)]],
    constant InstanceData* instances [[buffer(7)]],
    constant uint* materialData [[buffer(8)]]
) {
    VSOut out;
    uint4 entry = reflVisible[listIdx];
    Meshlet meshlet = meshlets[entry.x];
    InstanceData inst = instances[entry.y];

    uint triIndex = vertexID;
    if (triIndex >= meshlet.header.z * 3u) {
        out.position = float4(0.0);
        out.uv = float2(0.0);
        out.normalW = float3(0.0, 1.0, 0.0);
        out.albedoTint = float3(0.0);
        out.albedoIdx = 0u;
        return out;
    }

    uint packedTris = meshletTrianglesPacked[(meshlet.header.y + triIndex) >> 2u];
    uint localVertIdx = (packedTris >> (8u * ((meshlet.header.y + triIndex) & 3u))) & 0xFFu;
    uint vertIdx = meshletVertices[meshlet.header.x + localVertIdx];

    float3 pos = float3(positions[3u * vertIdx + 0u],
                        positions[3u * vertIdx + 1u],
                        positions[3u * vertIdx + 2u]);

    uint eBase = vertIdx * 6u;
    uint colorTSign = elements[eBase + 0u];
    uint packedNormal = elements[eBase + 1u];
    float2 uv = float2(as_type<float>(elements[eBase + 4u]),
                       as_type<float>(elements[eBase + 5u]));
    uv.y = 1.0 - uv.y;

    uint mBase = entry.z * 12u;
    out.albedoIdx = materialData[mBase + 0u];
    out.albedoTint = float3(as_type<float>(materialData[mBase + 3u]),
                            as_type<float>(materialData[mBase + 4u]),
                            as_type<float>(materialData[mBase + 5u]));
    float2 uvScale = float2(as_type<float>(materialData[mBase + 9u]),
                            as_type<float>(materialData[mBase + 10u]));
    out.uv = uv * uvScale;

    float4 worldPos = inst.world_matrix * float4(pos, 1.0);
    out.position = params.reflected_view_projection * worldPos;

    float3 n = unpackNormal(packedNormal, colorTSign);
    float3x3 normalMatrix = float3x3(inst.world_matrix[0].xyz,
                                     inst.world_matrix[1].xyz,
                                     inst.world_matrix[2].xyz);
    out.normalW = normalize(normalMatrix * n);
    return out;
}

fragment float4 mirror_reflection_fs(
    VSOut in [[stage_in]],
    constant ReflectionParams& params [[buffer(0)]],
    texture2d_array<float> albedoTextures [[texture(9)]],
    sampler albedoSampler [[sampler(10)]]
) {
    float3 albedo = albedoTextures.sample(albedoSampler, float3(in.uv, float(in.albedoIdx))).rgb
                  * in.albedoTint;
    float NdotL = max(dot(normalize(in.normalW), normalize(params.light_dir.xyz)), 0.0);
    float3 color = albedo * (params.ambient.rgb + params.light_color.rgb * NdotL);
    return float4(color, 1.0);
}
