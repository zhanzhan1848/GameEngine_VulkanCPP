// Vulkan reference: Nanite/GPUDrivenDraw.spv (gpu_driven_fragment_shader).
#include "../Common/TextureDimensions.hlsli"

struct DrawConstants
{
    row_major float4x4 view_matrix;
    row_major float4x4 proj_matrix;
    row_major float4x4 world_matrix;
    uint view_width;
    uint view_height;
    uint meshlet_count;
    uint padding;
    row_major float4x4 prev_view_matrix;
    row_major float4x4 prev_proj_matrix;
    uint has_prev_frame;
    uint debug_mode;
    uint pad2_1;
    uint pad2_2;
    float2 jitter;
    float2 prev_jitter;
};

struct Meshlet
{
    uint4 header;
    float4 cone0;
    float4 cone1;
    float4 cone2;
};

struct ClusterMap
{
    uint4 data;
};

struct InstanceData
{
    row_major float4x4 world_matrix;
    row_major float4x4 inverse_world_matrix;
    uint4 meta0;
    uint4 meta1;
    float bounds_center_x;
    float bounds_center_y;
    float bounds_center_z;
    uint _pad_after_center;
    float bounds_radius;
    uint _pad_br0;
    uint _pad_br1;
    uint _pad_br2;
};

struct VertexElement
{
    uint color_t_sign;
    uint normal;
    uint tangent;
    float2 uv;
};

struct MaterialInfo
{
    uint albedo_texture_idx;
    uint normal_texture_idx;
    uint orm_texture_idx;
    float3 albedo_tint;
    float metallic_factor;
    float roughness_factor;
    float normal_scale;
    float2 uv_scale;
    uint flags;
};

struct VSOutput
{
    float4 position;
    float2 uv;
    float3 normal;
    float3 tangent;
    float3 bitangent;
    uint material_id;
    uint meshlet_id;
    uint triangle_id;
    uint mesh_id;
    float4 current_clip;
    float4 previous_clip;
    float3 object_normal;
    float3 world_pos;
};

struct FSOutput
{
    float4 albedo;
    float4 normal;
    float4 orm;
    float2 velocity;
};

static const VertexElement _78 = { 0u, 0u, 0u, 0.0f.xx };
static const MaterialInfo _121 = { 0u, 0u, 0u, 0.0f.xxx, 0.0f, 0.0f, 0.0f, 0.0f.xx, 0u };
static const VSOutput _335 = { 0.0f.xxxx, 0.0f.xx, 0.0f.xxx, 0.0f.xxx, 0.0f.xxx, 0u, 0u, 0u, 0u, 0.0f.xxxx, 0.0f.xxxx, 0.0f.xxx, 0.0f.xxx };
static const FSOutput _644 = { 0.0f.xxxx, 0.0f.xxxx, 0.0f.xxxx, 0.0f.xx };

cbuffer uniforms : register(b0, space0)
{
    DrawConstants uniforms_1_m0 : packoffset(c0);
};

ByteAddressBuffer meshlets : register(t1, space0);
ByteAddressBuffer meshlet_vertices : register(t2, space0);
ByteAddressBuffer meshlet_triangles : register(t3, space0);
ByteAddressBuffer positions : register(t4, space0);
ByteAddressBuffer compact_cluster_ids : register(t5, space0);
ByteAddressBuffer cluster_map : register(t6, space0);
ByteAddressBuffer instance_data : register(t7, space0);
ByteAddressBuffer elements : register(t8, space0);
ByteAddressBuffer material_data : register(t9, space0);
Texture2DArray<float4> albedo_textures : register(t10, space0);
Texture2DArray<float4> normal_textures : register(t11, space0);
Texture2DArray<float4> orm_textures : register(t12, space0);
SamplerState texture_sampler : register(s13, space0);

static float4 fragCoord;
static float2 uv;
static float3 normal;
static float3 tangent;
static float3 bitangent;
static uint material_id;
static uint meshlet_id;
static uint triangle_id;
static uint mesh_id;
static float4 current_clip;
static float4 previous_clip;
static float3 object_normal;
static float3 world_pos;
static float4 albedo;
static float4 orm;
static float2 velocity;

struct StageInput
{
    float2 uv : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 tangent : TEXCOORD2;
    float3 bitangent : TEXCOORD3;
    nointerpolation uint material_id : TEXCOORD4;
    nointerpolation uint meshlet_id : TEXCOORD5;
    nointerpolation uint triangle_id : TEXCOORD6;
    nointerpolation uint mesh_id : TEXCOORD7;
    float4 current_clip : TEXCOORD8;
    float4 previous_clip : TEXCOORD9;
    float3 object_normal : TEXCOORD10;
    float3 world_pos : TEXCOORD11;
    float4 fragCoord : SV_Position;
    bool frontFacing : SV_IsFrontFace;
};

struct StageOutput
{
    float4 albedo : SV_Target0;
    float4 normal_1 : SV_Target1;
    float4 orm : SV_Target2;
    float2 velocity : SV_Target3;
};

MaterialInfo load_material(uint idx)
{
    MaterialInfo m = _121;
    uint _123 = idx * 12u;
    m.albedo_texture_idx = material_data.Load((_123 + 0u) * 4);
    m.normal_texture_idx = material_data.Load((_123 + 1u) * 4);
    m.orm_texture_idx = material_data.Load((_123 + 2u) * 4);
    m.albedo_tint = float3(asfloat(material_data.Load((_123 + 3u) * 4)), asfloat(material_data.Load((_123 + 4u) * 4)), asfloat(material_data.Load((_123 + 5u) * 4)));
    m.metallic_factor = asfloat(material_data.Load((_123 + 6u) * 4));
    m.roughness_factor = asfloat(material_data.Load((_123 + 7u) * 4));
    m.normal_scale = asfloat(material_data.Load((_123 + 8u) * 4));
    m.uv_scale = float2(asfloat(material_data.Load((_123 + 9u) * 4)), asfloat(material_data.Load((_123 + 10u) * 4)));
    m.flags = material_data.Load((_123 + 11u) * 4);
    return m;
}

float3 hash_id_to_color(uint id)
{
    uint h = 0u;
    h = id * 2654435761u;
    h ^= (h >> 16u);
    h *= 2246822507u;
    h ^= (h >> 13u);
    h *= 3266489909u;
    h ^= (h >> 16u);
    return float3(float(h & 255u) / 255.0f, float((h >> 8u) & 255u) / 255.0f, float((h >> 16u) & 255u) / 255.0f);
}

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    fragCoord = input.fragCoord;
    fragCoord.w = 1.0 / fragCoord.w;
    uv = input.uv;
    normal = input.normal;
    tangent = input.tangent;
    bitangent = input.bitangent;
    material_id = input.material_id;
    meshlet_id = input.meshlet_id;
    triangle_id = input.triangle_id;
    mesh_id = input.mesh_id;
    current_clip = input.current_clip;
    previous_clip = input.previous_clip;
    object_normal = input.object_normal;
    world_pos = input.world_pos;
    float metallic = 0.0f;
    float3x3 tbn = float3x3(0.0f.xxx, 0.0f.xxx, 0.0f.xxx);
    bool _648 = false;
    FSOutput _out = _644;
    bool _662 = false;
    float occlusion = 1.0f;
    float3 world_normal = 0.0f.xxx;
    float3 N = 0.0f.xxx;
    float roughness = 0.0f;
    bool _653 = false;
    float4 albedo_sample = 1.0f.xxxx;
    VSOutput _582 = { fragCoord, uv, normal, tangent, bitangent, material_id, meshlet_id, triangle_id, mesh_id, current_clip, previous_clip, object_normal, world_pos };
    if (_582.material_id == 4294967295u)
    {
        _out.albedo = float4(1.0f, 0.0f, 1.0f, 1.0f);
        _out.normal = float4((_582.normal * 0.5f) + 0.5f.xxx, 1.0f);
        _out.orm = float4(1.0f, 0.5f, 0.0f, 1.0f);
        _out.velocity = 0.0f.xx;
        albedo = _out.albedo;
        output.normal_1 = _out.normal;
        orm = _out.orm;
        velocity = _out.velocity;
        { output.albedo = albedo; output.orm = orm; output.velocity = velocity; return output; }
    }
    N = normalize(_582.normal);
    if (dot(N, normalize(cross(ddy(_582.world_pos), ddx(_582.world_pos)))) < 0.0f)
    {
        N = -N;
    }
    float3 _698 = normalize(_582.tangent);
    MaterialInfo _703 = load_material(_582.material_id);
    float2 _707 = frac(_582.uv * _703.uv_scale);

    if (_703.albedo_texture_idx != 4294967295u)
    {
        _648 = _703.albedo_texture_idx < textureDimensions(albedo_textures, 0u).z;
    }
    else
    {
        _648 = false;
    }
    if (_648)
    {
        albedo_sample = albedo_textures.SampleLevel(texture_sampler, float3(_707, float(_703.albedo_texture_idx)), 0.0f);
    }
    else
    {
        albedo_sample = float4(1.0f, 0.0f, 0.0f, 1.0f);
    }
    _out.albedo = float4(albedo_sample.xyz * _703.albedo_tint, 1.0f);
    world_normal = N;
    if (_703.normal_texture_idx != 4294967295u)
    {
        _653 = _703.normal_texture_idx < textureDimensions(normal_textures, 0u).z;
    }
    else
    {
        _653 = false;
    }
    if (_653)
    {
        float3 _754 = normal_textures.SampleLevel(texture_sampler, float3(_707, float(_703.normal_texture_idx)), 0.0f).xyz;
        if (length(_754) > 0.1f)
        {
            tbn[0u] = _698;
            tbn[1u] = normalize(cross(N, _698));
            tbn[2u] = N;
            world_normal = normalize(mul((_754 * 2.0f) - 1.0f.xxx, tbn));
        }
    }
    _out.normal = float4((world_normal * 0.5f) + 0.5f.xxx, 1.0f);
    roughness = max(_703.roughness_factor, 0.04f);
    metallic = _703.metallic_factor;
    if (_703.orm_texture_idx != 4294967295u)
    {
        _662 = _703.orm_texture_idx < textureDimensions(orm_textures, 0u).z;
    }
    else
    {
        _662 = false;
    }
    if (_662)
    {
        float4 _790 = orm_textures.SampleLevel(texture_sampler, float3(_707, float(_703.orm_texture_idx)), 0.0f);
        occlusion = _790.x;
        roughness = _790.y;
        metallic = _790.z;
    }
    _out.orm = float4(occlusion, roughness, metallic, float((_703.flags >> 8u) & 255u) / 255.0f);
    if (uniforms_1_m0.has_prev_frame != 0u)
    {
        _out.velocity = (_582.current_clip.xy / _582.current_clip.w.xx) - (_582.previous_clip.xy / _582.previous_clip.w.xx);
    }
    else
    {
        _out.velocity = 0.0f.xx;
    }
    if (uniforms_1_m0.debug_mode != 0u)
    {
        if (uniforms_1_m0.debug_mode == 4u)
        {
            _out.albedo = float4((N * 0.5f) + 0.5f.xxx, 1.0f);
            _out.normal = float4(0.5f, 0.5f, 1.0f, 1.0f);
            _out.orm = float4(1.0f, 1.0f, 0.0f, 1.0f);
        }
        else
        {
            if (uniforms_1_m0.debug_mode == 5u)
            {
                _out.albedo = float4((normalize(_582.object_normal) * 0.5f) + 0.5f.xxx, 1.0f);
                _out.normal = float4(0.5f, 0.5f, 1.0f, 1.0f);
                _out.orm = float4(1.0f, 1.0f, 0.0f, 1.0f);
            }
            else
            {
                _out.albedo = float4(hash_id_to_color((uniforms_1_m0.debug_mode != 1u) ? ((uniforms_1_m0.debug_mode == 3u) ? _582.mesh_id : _582.triangle_id) : _582.meshlet_id), 1.0f);
                _out.normal = float4(0.5f, 0.5f, 1.0f, 1.0f);
                _out.orm = float4(1.0f, 0.9f, 0.0f, 1.0f);
            }
        }
    }
    albedo = _out.albedo;
    output.normal_1 = _out.normal;
    orm = _out.orm;
    velocity = _out.velocity;
    output.albedo = albedo;
    output.orm = orm;
    output.velocity = velocity;
    return output;
}
