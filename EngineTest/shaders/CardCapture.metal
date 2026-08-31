// CardCapture.metal — T8.12 card-view capture (Metal port of the Vulkan
// Nanite/CardCapture.vert/.frag pair; T8.13 Vulkan↔Metal sync item 8).
//
// Draws scene geometry from one Surface Cache card's orthographic view into
// the card's atlas pages (MRT: albedo / oct-normal / axis-depth).
//
// Geometry decode is semantically identical to the Vulkan GBuffer path:
// visible-cluster list (binding 5, CPU-built per face) → cluster map →
// meshlet / instance / material. Deltas vs the main-camera shaders:
//   - binding 0 is a card Params UBO (ortho view/proj + card frame);
//   - VS outputs world_pos so the PS can emit the card-axis depth
//     (worldToCardUV semantics: dot(local, fwd), positive into the view);
//   - cull nothing (cloth/thin geometry must capture from both windings —
//     CullMode::None is set on the shared C++ pipeline).
//
// Metal adaptation notes (behavior-neutral):
//   - One file, two named entries: card_capture_vertex / card_capture_
//     fragment. MSL libraries cannot host two functions named "main", and
//     the RHI resolves functions by name from the compiled source
//     (MetalShader::Initialize → newFunction).
//   - gl_VertexIndex / gl_InstanceIndex → [[vertex_id]] / [[instance_id]]
//     (DrawIndirect instanceCount = visible-cluster count works identically).
//   - Binding slots follow the shared C++ layout verbatim: buffers 0-9
//     (binding index = Metal slot), albedo array [[texture(10)]],
//     sampler [[sampler(11)]], MRT0/1/2 = rgba8 / rg16f / r32f.
//
// Dispatched by GPUDrivenDrawPipeline::ExecuteCardCapturePass — RT formats,
// viewport, and atlas addressing all come from the shared C++ path.

#include <metal_stdlib>
using namespace metal;

// Matches CardCaptureParamsCB (176 bytes): view, proj, center, fwd, misc.
struct CardParams {
    float4x4 view;     // card ortho view (world → card view space)
    float4x4 proj;     // card ortho projection
    float4   center;   // card center, xyz
    float4   fwd;      // card forward axis, xyz (unit, sign folded per dir)
    float4   misc;     // x = depth_near, y = depth_far
};

// Matches the Vulkan Meshlet struct (80 bytes).
struct CardMeshlet {
    uint4  header;   // x=vertex_offset, y=triangle_offset, z=vert_count, w=tri_count
    float4 cone0;
    float4 cone1;
    float4 cone2;
};

// Matches the Vulkan InstanceData struct (192 bytes): the C++ math::v3
// bounds_center (12B + 4B pad) is carried as an float4, and the radius
// + 12B pad as a second float4 — byte-identical.
struct CardInstanceData {
    float4x4 world_matrix;            //   0
    float4x4 inverse_world_matrix;    //  64
    uint4    meta0;                   // 128
    uint4    meta1;                   // 144
    float4   bounds_center_pad;       // 160: xyz = bounds center
    float4   bounds_radius_pad;       // 176: x = bounds radius
};

struct CardCaptureVSOut {
    float4 position      [[position]];
    float2 uv            [[user(locn0)]];
    float3 normal        [[user(locn1)]];
    uint   material_id   [[user(locn2), flat]];
    float3 world_pos     [[user(locn3)]];
};

// Packed 16-bit xy + sign byte — identical decode to PipelineDraw/.CardCapture.vert.
static float3 cardUnpackNormal(uint packed, uint color_t_sign) {
    float hi = float((packed >> 16u) & 0xFFFFu);
    float lo = float(packed & 0xFFFFu);
    float inv = 2.0f / 65535.0f;
    float2 f = float2(hi * inv - 1.0f, lo * inv - 1.0f);
    float d = dot(f, f);
    if (d > 1.0f) return float3(0.0f, 0.0f, 1.0f);
    float z = sqrt(max(0.0f, 1.0f - d));
    uint signs = (color_t_sign >> 24u) & 0xFFu;
    float nSign = float(signs & 0x02u) - 1.0f;
    return float3(f.x, f.y, z * nSign);
}

vertex CardCaptureVSOut card_capture_vertex(
    uint vid [[vertex_id]],
    uint instance_id [[instance_id]],

    constant CardParams&         cb   [[buffer(0)]],
    device const CardMeshlet*    mls  [[buffer(1)]],
    device const uint*           mvs  [[buffer(2)]],
    device const uint*           mts  [[buffer(3)]],
    device const float*          pos  [[buffer(4)]],
    device const uint*           cids [[buffer(5)]],
    device const uint4*          cmap [[buffer(6)]],
    device const CardInstanceData* insts [[buffer(7)]],
    device const uint*           elems [[buffer(8)]])
{
    CardCaptureVSOut OUT;

    uint global_cluster_id = cids[instance_id];
    uint4 cm = cmap[global_cluster_id];
    uint global_meshlet_index = cm.x;
    uint instance_index = cm.y;
    uint material_id = cm.z;

    CardInstanceData inst = insts[instance_index];
    CardMeshlet meshlet = mls[global_meshlet_index];

    uint triangle_count = meshlet.header.w;
    uint actual_vertex_count = triangle_count * 3u;

    if (vid >= actual_vertex_count) {
        OUT.uv = float2(0.0f);
        OUT.normal = float3(0.0f, 1.0f, 0.0f);
        OUT.material_id = material_id;
        OUT.world_pos = float3(0.0f);
        OUT.position = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return OUT;
    }

    uint tri_offset = meshlet.header.y;
    uint local_vertex_idx = mts[tri_offset + vid];
    uint vert_offset = meshlet.header.x;
    uint vert_idx = mvs[vert_offset + local_vertex_idx];

    float3 p = float3(
        pos[vert_idx * 3u + 0u],
        pos[vert_idx * 3u + 1u],
        pos[vert_idx * 3u + 2u]);

    uint elem_base = vert_idx * 6u;
    uint color_t_sign = elems[elem_base + 0u];
    uint packed_normal = elems[elem_base + 1u];
    float2 uv = float2(as_type<float>(elems[elem_base + 4u]),
                       as_type<float>(elems[elem_base + 5u]));

    float3 normal = cardUnpackNormal(packed_normal, color_t_sign);

    float4 world_pos = inst.world_matrix * float4(p, 1.0f);
    // Upper 3×3 (uniform-scale model matrices — same recipe as the GLSL port).
    float3x3 normal_matrix = float3x3(
        inst.world_matrix[0].xyz,
        inst.world_matrix[1].xyz,
        inst.world_matrix[2].xyz);
    float3 world_normal = normalize(normal_matrix * normal);

    uv.y = 1.0f - uv.y;
    OUT.uv = uv;
    OUT.normal = world_normal;
    OUT.material_id = material_id;
    OUT.world_pos = world_pos.xyz;

    OUT.position = cb.proj * (cb.view * world_pos);
    return OUT;
}

// Material SSBO entry stride: 12 uints (48 B) — albedo idx at 0, tint at 3..5.
struct CardMaterialInfo {
    uint  albedo_texture_idx;
    float3 albedo_tint;
};

static CardMaterialInfo cardLoadMaterial(device const uint* mats, uint idx) {
    uint base = idx * 12u;
    CardMaterialInfo m;
    m.albedo_texture_idx = mats[base + 0u];
    m.albedo_tint = float3(
        as_type<float>(mats[base + 3u]),
        as_type<float>(mats[base + 4u]),
        as_type<float>(mats[base + 5u]));
    return m;
}

static float2 cardOctEncode(float3 n) {
    float l1norm = abs(n.x) + abs(n.y) + abs(n.z);
    float2 result = n.xy / l1norm;
    if (n.z < 0.0f) {
        result = (1.0f - abs(result.yx)) * select(float2(-1.0f), float2(1.0f), result.xy >= float2(0.0f));
    }
    return result * 0.5f + 0.5f;
}

struct CardCapturePSOut {
    float4 albedo     [[color(0)]];   // rgba8  = base_color, alpha 1
    float4 normal     [[color(1)]];   // rg16f  = oct-encoded world normal
    float  axis_depth [[color(2)]];   // r32f   = dot(world_pos - center, fwd)
};

fragment CardCapturePSOut card_capture_fragment(
    CardCaptureVSOut IN              [[stage_in]],
    bool front_facing                [[front_facing]],

    constant CardParams& cb          [[buffer(0)]],
    device const uint* mats          [[buffer(9)]],
    texture2d_array<float> albedo_textures [[texture(10)]],
    sampler texture_sampler          [[sampler(11)]])
{
    // Missing-material sentinel: neutral gray (never magenta — atlas texels
    // feed probes).
    CardMaterialInfo mat = cardLoadMaterial(mats, IN.material_id);
    float2 scaled_uv = fract(IN.uv);

    float4 albedo_sample = float4(0.5f);
    int albedo_layers = (int)albedo_textures.get_array_size();
    if (mat.albedo_texture_idx != 0xFFFFFFFFu && int(mat.albedo_texture_idx) < albedo_layers) {
        albedo_sample = albedo_textures.sample(texture_sampler, scaled_uv, mat.albedo_texture_idx);
    }
    float3 base_color = albedo_sample.rgb * mat.albedo_tint;

    float3 N = normalize(IN.normal);
    if (!front_facing) N = -N;

    CardCapturePSOut OUT;
    OUT.albedo = float4(base_color, 1.0f);
    OUT.normal = float4(cardOctEncode(N), 0.0f, 1.0f);
    OUT.axis_depth = dot(IN.world_pos - cb.center.xyz, cb.fwd.xyz);
    return OUT;
}
