// Vulkan reference: Nanite/MirrorReflection.vert (main).
struct MeshletHeader
{
    uint vertex_offset;
    uint triangle_offset;
    uint vertex_count;
    uint triangle_count;
    float4 cone0;
    float4 cone1;
    float4 cone2;
};

struct InstanceData
{
    row_major float4x4 world_matrix;
    row_major float4x4 inverse_world_matrix;
    uint4 meta0;
    uint4 meta1;
    float4 bounds_center_pad;
    float4 bounds_radius_pad;
};

ByteAddressBuffer triIndices : register(t3, space0);
ByteAddressBuffer reflVisible : register(t6, space0);
ByteAddressBuffer meshletBuffer : register(t1, space0);
ByteAddressBuffer instanceBuffer : register(t7, space0);
ByteAddressBuffer vertexIndices : register(t2, space0);
ByteAddressBuffer positions : register(t4, space0);
ByteAddressBuffer elements : register(t5, space0);
ByteAddressBuffer materialData : register(t8, space0);
cbuffer ReflectionParams : register(b0, space0)
{
    row_major float4x4 reflected_view_projection : packoffset(c0);
    float4 light_dir : packoffset(c4);
    float4 light_color : packoffset(c5);
    float4 ambient : packoffset(c6);
};

struct StageInput
{
    uint vertexID : SV_VertexID;
    uint instanceID : SV_InstanceID;
};

struct StageOutput
{
    float2 vUV : TEXCOORD0;
    float3 vNormalW : TEXCOORD1;
    float3 vAlbedoTint : TEXCOORD2;
    nointerpolation uint vAlbedoIdx : TEXCOORD3;
    float4 positionCS : SV_Position;
};

uint readTriangleByte(uint byteIndex)
{
    uint _packed = triIndices.Load((byteIndex >> 2u) * 4);
    return (_packed >> (8u * (byteIndex & 3u))) & 255u;
}

float3 unpackNormal(uint packed, uint colorTSign) {
    float hi = float((packed >> 16u) & 0xFFFFu);
    float lo = float(packed & 0xFFFFu);
    float2 f = float2(hi, lo) * (2.0 / 65535.0) - (float2)(1.0);
    float d = dot(f, f);
    if (d > 1.0) return float3(0.0, 0.0, 1.0);
    float z = sqrt(max(0.0, 1.0 - d));
    uint signs = (colorTSign >> 24u) & 0xFFu;
    float nSign = float(signs & 0x02u) - 1.0;
    return float3(f.x, f.y, z * nSign);
}

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    uint vertexID = uint(int(input.vertexID));
    uint listIdx = uint(int(input.instanceID));
    uint4 entry = reflVisible.Load4(listIdx * 16);
    MeshletHeader _138;
    _138.vertex_offset = meshletBuffer.Load(entry.x * 64);
    _138.triangle_offset = meshletBuffer.Load(entry.x * 64 + 4);
    _138.vertex_count = meshletBuffer.Load(entry.x * 64 + 8);
    _138.triangle_count = meshletBuffer.Load(entry.x * 64 + 12);
    _138.cone0 = asfloat(meshletBuffer.Load4(entry.x * 64 + 16));
    _138.cone1 = asfloat(meshletBuffer.Load4(entry.x * 64 + 32));
    _138.cone2 = asfloat(meshletBuffer.Load4(entry.x * 64 + 48));
    MeshletHeader meshlet;
    meshlet.vertex_offset = _138.vertex_offset;
    meshlet.triangle_offset = _138.triangle_offset;
    meshlet.vertex_count = _138.vertex_count;
    meshlet.triangle_count = _138.triangle_count;
    meshlet.cone0 = _138.cone0;
    meshlet.cone1 = _138.cone1;
    meshlet.cone2 = _138.cone2;
    InstanceData _173;
    _173.world_matrix = asfloat(uint4x4(instanceBuffer.Load4(entry.y * 192), instanceBuffer.Load4(entry.y * 192 + 16), instanceBuffer.Load4(entry.y * 192 + 32), instanceBuffer.Load4(entry.y * 192 + 48)));
    _173.inverse_world_matrix = asfloat(uint4x4(instanceBuffer.Load4(entry.y * 192 + 64), instanceBuffer.Load4(entry.y * 192 + 80), instanceBuffer.Load4(entry.y * 192 + 96), instanceBuffer.Load4(entry.y * 192 + 112)));
    _173.meta0 = instanceBuffer.Load4(entry.y * 192 + 128);
    _173.meta1 = instanceBuffer.Load4(entry.y * 192 + 144);
    _173.bounds_center_pad = asfloat(instanceBuffer.Load4(entry.y * 192 + 160));
    _173.bounds_radius_pad = asfloat(instanceBuffer.Load4(entry.y * 192 + 176));
    InstanceData inst;
    inst.world_matrix = _173.world_matrix;
    inst.inverse_world_matrix = _173.inverse_world_matrix;
    inst.meta0 = _173.meta0;
    inst.meta1 = _173.meta1;
    inst.bounds_center_pad = _173.bounds_center_pad;
    inst.bounds_radius_pad = _173.bounds_radius_pad;
    uint triIndex = vertexID;
    if (triIndex >= (meshlet.triangle_count * 3u))
    {
        output.positionCS = 0.0f.xxxx;
        output.vUV = 0.0f.xx;
        output.vNormalW = float3(0.0f, 1.0f, 0.0f);
        output.vAlbedoTint = 0.0f.xxx;
        output.vAlbedoIdx = 0u;
        return output;
    }
    uint localVertIdx = readTriangleByte(meshlet.triangle_offset + triIndex);
    uint vertIdx = vertexIndices.Load((meshlet.vertex_offset + localVertIdx) * 4);
    float3 pos = float3(asfloat(positions.Load(((3u * vertIdx) + 0u) * 4)), asfloat(positions.Load(((3u * vertIdx) + 1u) * 4)), asfloat(positions.Load(((3u * vertIdx) + 2u) * 4)));
    uint eBase = vertIdx * 6u;
    uint colorTSign = elements.Load((eBase + 0u) * 4);
    uint packedNormal = elements.Load((eBase + 1u) * 4);
    float2 uv = float2(asfloat(elements.Load((eBase + 4u) * 4)), asfloat(elements.Load((eBase + 5u) * 4)));
    uv.y = 1.0f - uv.y;
    uint mBase = entry.z * 12u;
    output.vAlbedoIdx = materialData.Load((mBase + 0u) * 4);
    output.vAlbedoTint = float3(asfloat(materialData.Load((mBase + 3u) * 4)), asfloat(materialData.Load((mBase + 4u) * 4)), asfloat(materialData.Load((mBase + 5u) * 4)));
    float2 uvScale = float2(asfloat(materialData.Load((mBase + 9u) * 4)), asfloat(materialData.Load((mBase + 10u) * 4)));
    output.vUV = uv * uvScale;
    float4 worldPos = mul(float4(pos, 1.0f), inst.world_matrix);
    output.positionCS = mul(worldPos, reflected_view_projection);
    uint param_1 = packedNormal;
    uint param_2 = colorTSign;
    float3 n = unpackNormal(param_1, param_2);
    float3x3 normalMatrix = float3x3(float3(inst.world_matrix[0].xyz), float3(inst.world_matrix[1].xyz), float3(inst.world_matrix[2].xyz));
    output.vNormalW = normalize(mul(n, normalMatrix));
    return output;
}
