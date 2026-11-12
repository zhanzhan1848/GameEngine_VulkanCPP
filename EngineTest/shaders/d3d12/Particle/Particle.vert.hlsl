struct ParticleData
{
    float4 position;
    float4 velocity;
    float4 color;
    float4 scale_rotation;
    float4 uv_params;
};

ByteAddressBuffer visibleCount : register(t2, space0);
ByteAddressBuffer particleData : register(t1, space0);
cbuffer ParticleUniforms : register(b0, space0)
{
    row_major float4x4 pc_view_projection : packoffset(c0);
    row_major float4x4 pc_view_matrix : packoffset(c4);
    float pc_time : packoffset(c8);
    float pc_delta_time : packoffset(c8.y);
    uint pc_blend_mode : packoffset(c8.z);
    uint pc_texture_frames_x : packoffset(c8.w);
    uint pc_texture_frames_y : packoffset(c9);
    float pc_frame_rate : packoffset(c9.y);
};


static float4 gl_Position;
static int gl_VertexIndex;
static int gl_InstanceIndex;
static float4 outColor;
static float2 outUV;
static float outAge;

struct SPIRV_Cross_Input
{
    uint gl_VertexIndex : SV_VertexID;
    uint gl_InstanceIndex : SV_InstanceID;
};

struct SPIRV_Cross_Output
{
    float4 outColor : TEXCOORD0;
    float2 outUV : TEXCOORD1;
    float outAge : TEXCOORD2;
    float4 gl_Position : SV_Position;
};

static float2 quad_positions[6];
static float2 quad_uvs[6];

float mod(float x, float y)
{
    return x - y * floor(x / y);
}

float2 mod(float2 x, float2 y)
{
    return x - y * floor(x / y);
}

float3 mod(float3 x, float3 y)
{
    return x - y * floor(x / y);
}

float4 mod(float4 x, float4 y)
{
    return x - y * floor(x / y);
}

void initQuadTables()
{
    quad_positions[0] = (-0.5f).xx;
    quad_positions[1] = float2(0.5f, -0.5f);
    quad_positions[2] = float2(-0.5f, 0.5f);
    quad_positions[3] = float2(-0.5f, 0.5f);
    quad_positions[4] = float2(0.5f, -0.5f);
    quad_positions[5] = 0.5f.xx;
    quad_uvs[0] = 0.0f.xx;
    quad_uvs[1] = float2(1.0f, 0.0f);
    quad_uvs[2] = float2(0.0f, 1.0f);
    quad_uvs[3] = float2(0.0f, 1.0f);
    quad_uvs[4] = float2(1.0f, 0.0f);
    quad_uvs[5] = 1.0f.xx;
}

void vert_main()
{
    initQuadTables();
    uint vertex_id = uint(gl_VertexIndex);
    uint instance_id = uint(gl_InstanceIndex);
    if (instance_id >= visibleCount.Load(0))
    {
        gl_Position = float4(0.0f, 0.0f, -1000.0f, 1.0f);
        outColor = 0.0f.xxxx;
        outUV = 0.0f.xx;
        outAge = 1.0f;
        return;
    }
    ParticleData _98;
    _98.position = asfloat(particleData.Load4(instance_id * 80 + 0));
    _98.velocity = asfloat(particleData.Load4(instance_id * 80 + 16));
    _98.color = asfloat(particleData.Load4(instance_id * 80 + 32));
    _98.scale_rotation = asfloat(particleData.Load4(instance_id * 80 + 48));
    _98.uv_params = asfloat(particleData.Load4(instance_id * 80 + 64));
    ParticleData p;
    p.position = _98.position;
    p.velocity = _98.velocity;
    p.color = _98.color;
    p.scale_rotation = _98.scale_rotation;
    p.uv_params = _98.uv_params;
    float2 scale = p.scale_rotation.xy;
    float2 quad_pos = quad_positions[vertex_id];
    float2 quad_uv = quad_uvs[vertex_id];
    float3 camera_right = float3(pc_view_matrix[0].x, pc_view_matrix[1].x, pc_view_matrix[2].x);
    float3 camera_up = float3(pc_view_matrix[0].y, pc_view_matrix[1].y, pc_view_matrix[2].y);
    float3 world_pos = p.position.xyz;
    float3 offset = ((camera_right * quad_pos.x) * scale.x) + ((camera_up * quad_pos.y) * scale.y);
    float2 debug_offsets[6];
    debug_offsets[0] = (-0.5f).xx;
    debug_offsets[1] = float2(0.5f, -0.5f);
    debug_offsets[2] = float2(-0.5f, 0.5f);
    debug_offsets[3] = float2(-0.5f, 0.5f);
    debug_offsets[4] = float2(0.5f, -0.5f);
    debug_offsets[5] = 0.5f.xx;
    gl_Position = float4(debug_offsets[vertex_id] * 2.0f, 1.0f, 1.0f);
    float atlas_index = p.uv_params.x;
    uint cols = pc_texture_frames_x;
    uint rows = pc_texture_frames_y;
    if ((cols > 1u) || (rows > 1u))
    {
        float tile_x = mod(atlas_index, float(cols));
        float tile_y = floor(atlas_index / float(cols));
        float2 tile_size = 1.0f.xx / float2(float(cols), float(rows));
        float2 tile_offset = tile_size * float2(tile_x, tile_y);
        outUV = tile_offset + (quad_uv * tile_size);
    }
    else
    {
        outUV = quad_uv;
    }
    float age = p.position.w;
    float alpha_fade = 1.0f - smoothstep(0.699999988079071044921875f, 1.0f, age);
    outColor = float4(p.color.xyz, p.color.w * alpha_fade);
    outAge = age;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_VertexIndex = int(stage_input.gl_VertexIndex);
    gl_InstanceIndex = int(stage_input.gl_InstanceIndex);
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.outColor = outColor;
    stage_output.outUV = outUV;
    stage_output.outAge = outAge;
    return stage_output;
}
