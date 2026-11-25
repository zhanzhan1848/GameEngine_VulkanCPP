// Vulkan reference: Nanite/GPUDrivenDraw.spv (gpu_driven_vertex_shader).
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

struct StageInput
{
    uint vertexID : SV_VertexID;
    uint instanceID : SV_InstanceID;
};

struct StageOutput
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
    float4 positionCS : SV_Position;
};

uint naga_div(uint lhs, uint rhs)
{
    return lhs / ((rhs == 0u) ? 1u : rhs);
}

uint naga_mod(uint lhs, uint rhs)
{
    return lhs % ((rhs == 0u) ? 1u : rhs);
}

VertexElement load_vertex_element(uint idx)
{
    VertexElement e = _78;
    uint _80 = idx * 6u;
    e.color_t_sign = elements.Load((_80 + 0u) * 4);
    e.normal = elements.Load((_80 + 1u) * 4);
    e.tangent = elements.Load((_80 + 2u) * 4);
    e.uv = float2(asfloat(elements.Load((_80 + 4u) * 4)), asfloat(elements.Load((_80 + 5u) * 4)));
    return e;
}

float3 unpack_normal(uint _packed, uint color_t_sign)
{
    float2 f = 0.0f.xx;
    f = float2((float((_packed >> 16u) & 65535u) * 3.0518044e-05f) - 1.0f, (float(_packed & 65535u) * 3.0518044e-05f) - 1.0f);
    float _210 = dot(f, f);
    if (_210 > 1.0f)
    {
        return float3(0.0f, 0.0f, 1.0f);
    }
    return float3(f.x, f.y, sqrt(max(0.0f, 1.0f - _210)) * (float(((color_t_sign >> 24u) & 255u) & 2u) - 1.0f));
}

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float3x3 normal_matrix = float3x3(0.0f.xxx, 0.0f.xxx, 0.0f.xxx);
    float3 world_tangent = 0.0f.xxx;
    VSOutput _out = _335;
    float3 world_normal = 0.0f.xxx;
    ClusterMap _348;
    _348.data = cluster_map.Load4(compact_cluster_ids.Load(uint(int(input.instanceID)) * 4) * 16);
    InstanceData _357;
    _357.world_matrix = asfloat(uint4x4(instance_data.Load4(_348.data.y * 192), instance_data.Load4(_348.data.y * 192 + 16), instance_data.Load4(_348.data.y * 192 + 32), instance_data.Load4(_348.data.y * 192 + 48)));
    _357.inverse_world_matrix = asfloat(uint4x4(instance_data.Load4(_348.data.y * 192 + 64), instance_data.Load4(_348.data.y * 192 + 80), instance_data.Load4(_348.data.y * 192 + 96), instance_data.Load4(_348.data.y * 192 + 112)));
    _357.meta0 = instance_data.Load4(_348.data.y * 192 + 128);
    _357.meta1 = instance_data.Load4(_348.data.y * 192 + 144);
    _357.bounds_center_x = asfloat(instance_data.Load(_348.data.y * 192 + 160));
    _357.bounds_center_y = asfloat(instance_data.Load(_348.data.y * 192 + 164));
    _357.bounds_center_z = asfloat(instance_data.Load(_348.data.y * 192 + 168));
    _357._pad_after_center = instance_data.Load(_348.data.y * 192 + 172);
    _357.bounds_radius = asfloat(instance_data.Load(_348.data.y * 192 + 176));
    _357._pad_br0 = instance_data.Load(_348.data.y * 192 + 180);
    _357._pad_br1 = instance_data.Load(_348.data.y * 192 + 184);
    _357._pad_br2 = instance_data.Load(_348.data.y * 192 + 188);
    Meshlet _360;
    _360.header = meshlets.Load4(_348.data.x * 64);
    _360.cone0 = asfloat(meshlets.Load4(_348.data.x * 64 + 16));
    _360.cone1 = asfloat(meshlets.Load4(_348.data.x * 64 + 32));
    _360.cone2 = asfloat(meshlets.Load4(_348.data.x * 64 + 48));
    if (uint(int(input.vertexID)) >= (_360.header.w * 3u))
    {
        _out.position = 0.0f.xxxx;
        _out.uv = 0.0f.xx;
        _out.normal = float3(0.0f, 0.0f, 1.0f);
        _out.tangent = float3(1.0f, 0.0f, 0.0f);
        _out.bitangent = float3(0.0f, 1.0f, 0.0f);
        _out.material_id = _348.data.z;
        _out.meshlet_id = _348.data.x;
        _out.triangle_id = 0u;
        _out.mesh_id = _348.data.y;
        _out.current_clip = _out.position;
        _out.previous_clip = _out.position;
        _out.object_normal = float3(0.0f, 1.0f, 0.0f);
        output.positionCS = _out.position;
        output.positionCS.y = -output.positionCS.y;
        uv = _out.uv;
        normal = _out.normal;
        tangent = _out.tangent;
        bitangent = _out.bitangent;
        material_id = _out.material_id;
        meshlet_id = _out.meshlet_id;
        triangle_id = _out.triangle_id;
        mesh_id = _out.mesh_id;
        current_clip = _out.current_clip;
        previous_clip = _out.previous_clip;
        object_normal = _out.object_normal;
        world_pos = _out.world_pos;
        { output.uv = uv; output.normal = normal; output.tangent = tangent; output.bitangent = bitangent; output.material_id = material_id; output.meshlet_id = meshlet_id; output.triangle_id = triangle_id; output.mesh_id = mesh_id; output.current_clip = current_clip; output.previous_clip = previous_clip; output.object_normal = object_normal; output.world_pos = world_pos; return output; }
    }
    uint _404 = _360.header.y + uint(int(input.vertexID));
    uint _414 = _360.header.x + ((meshlet_triangles.Load(naga_div(_404, 4u) * 4) >> (naga_mod(_404, 4u) * 8u)) & 255u);
    VertexElement _430 = load_vertex_element(meshlet_vertices.Load(_414 * 4));
    float3 _433 = unpack_normal(_430.normal, _430.color_t_sign);
    float4 _449 = mul(float4(float3(asfloat(positions.Load((meshlet_vertices.Load(_414 * 4) * 3u) * 4)), asfloat(positions.Load(((meshlet_vertices.Load(_414 * 4) * 3u) + 1u) * 4)), asfloat(positions.Load(((meshlet_vertices.Load(_414 * 4) * 3u) + 2u) * 4))), 1.0f), _357.world_matrix);
    _out.position = mul(mul(_449, uniforms_1_m0.view_matrix), uniforms_1_m0.proj_matrix);
    _out.position = float4(_out.position.xy + (uniforms_1_m0.jitter * _out.position.w), _out.position.zw);
    _out.current_clip = _out.position;
    if (uniforms_1_m0.has_prev_frame != 0u)
    {
        float4 _488 = mul(mul(_449, uniforms_1_m0.prev_view_matrix), uniforms_1_m0.prev_proj_matrix);
        _out.previous_clip = float4(_488.xy + (uniforms_1_m0.prev_jitter * _488.w), _488.zw);
    }
    else
    {
        _out.previous_clip = _out.current_clip;
    }
    _out.world_pos = _449.xyz;
    normal_matrix[0u] = _357.world_matrix[0].xyz;
    normal_matrix[1u] = _357.world_matrix[1].xyz;
    normal_matrix[2u] = _357.world_matrix[2].xyz;
    _out.object_normal = _433;
    world_normal = normalize(mul(_433, normal_matrix));
    world_tangent = normalize(mul(unpack_normal(_430.tangent, _430.color_t_sign), normal_matrix));
    if (abs(dot(world_tangent, world_normal)) > 0.999f)
    {
        bool3 _537 = (abs(world_normal.y) < 0.999f).xxx;
        world_tangent = normalize(cross(float3(_537.x ? float3(1.0f, 0.0f, 0.0f).x : float3(0.0f, 1.0f, 0.0f).x, _537.y ? float3(1.0f, 0.0f, 0.0f).y : float3(0.0f, 1.0f, 0.0f).y, _537.z ? float3(1.0f, 0.0f, 0.0f).z : float3(0.0f, 1.0f, 0.0f).z), world_normal));
    }
    else
    {
        world_tangent = normalize(world_tangent - (world_normal * dot(world_tangent, world_normal)));
    }
    _out.uv = float2(_430.uv.x, 1.0f - _430.uv.y);
    _out.normal = world_normal;
    _out.tangent = world_tangent;
    _out.bitangent = cross(world_normal, world_tangent) * (((_430.color_t_sign & 4278190080u) != 0u) ? (-1.0f) : 1.0f);
    _out.material_id = _348.data.z;
    _out.meshlet_id = _348.data.x;
    _out.triangle_id = naga_div(uint(int(input.vertexID)), 3u);
    _out.mesh_id = _348.data.y;
    output.positionCS = _out.position;
    output.positionCS.y = -output.positionCS.y;
    uv = _out.uv;
    normal = _out.normal;
    tangent = _out.tangent;
    bitangent = _out.bitangent;
    material_id = _out.material_id;
    meshlet_id = _out.meshlet_id;
    triangle_id = _out.triangle_id;
    mesh_id = _out.mesh_id;
    current_clip = _out.current_clip;
    previous_clip = _out.previous_clip;
    object_normal = _out.object_normal;
    world_pos = _out.world_pos;
    output.uv = uv;
    output.normal = normal;
    output.tangent = tangent;
    output.bitangent = bitangent;
    output.material_id = material_id;
    output.meshlet_id = meshlet_id;
    output.triangle_id = triangle_id;
    output.mesh_id = mesh_id;
    output.current_clip = current_clip;
    output.previous_clip = previous_clip;
    output.object_normal = object_normal;
    output.world_pos = world_pos;
    return output;
}
