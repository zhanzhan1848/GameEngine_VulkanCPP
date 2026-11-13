static const uint3 gl_WorkGroupSize = uint3(4u, 4u, 4u);

cbuffer CascadeParams : register(b8, space0)
{
    float4 cp_origin_spacing : packoffset(c0);
    float4 cp_counts_enabled : packoffset(c1);
    float4 cp_ddgi_origin_spacing : packoffset(c2);
    float4 cp_ddgi_counts : packoffset(c3);
    float cp_temporal_weight : packoffset(c4);
    float cp_frame_index : packoffset(c4.y);
    float cp_pad0 : packoffset(c4.z);
    float cp_pad1 : packoffset(c4.w);
};

ByteAddressBuffer ddgiIrr : register(t9, space0);
RWTexture3D<float4> cascade_out_l0 : register(u0, space0);
RWTexture3D<float4> cascade_out_l1x : register(u1, space0);
RWTexture3D<float4> cascade_out_l1y : register(u2, space0);
RWTexture3D<float4> cascade_out_l1z : register(u3, space0);
Texture3D<float4> cascade_prev_l0 : register(t4, space0);
Texture3D<float4> cascade_prev_l1x : register(t5, space0);
Texture3D<float4> cascade_prev_l1y : register(t6, space0);
Texture3D<float4> cascade_prev_l1z : register(t7, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

void comp_main()
{
    uint3 tid = gl_GlobalInvocationID;
    if (any(bool3(tid.x >= uint3(32u, 32u, 32u).x, tid.y >= uint3(32u, 32u, 32u).y, tid.z >= uint3(32u, 32u, 32u).z)))
    {
        return;
    }
    float3 sh[4];
    if (cp_counts_enabled.w < 0.5f)
    {
        sh[0] = 0.0f.xxx;
        sh[1] = 0.0f.xxx;
        sh[2] = 0.0f.xxx;
        sh[3] = 0.0f.xxx;
        cascade_out_l0[int3(tid)] = float4(sh[0], 1.0f);
        cascade_out_l1x[int3(tid)] = float4(sh[1], 1.0f);
        cascade_out_l1y[int3(tid)] = float4(sh[2], 1.0f);
        cascade_out_l1z[int3(tid)] = float4(sh[3], 1.0f);
        return;
    }
    float3 worldPos = cp_origin_spacing.xyz + ((float3(tid) + 0.5f.xxx) * cp_origin_spacing.w);
    float3 gp = (worldPos - cp_ddgi_origin_spacing.xyz) / cp_ddgi_origin_spacing.w.xxx;
    float3 gridMax = cp_ddgi_counts.xyz - 1.0f.xxx;
    bool _129 = any(bool3(gp.x < 0.0f.xxx.x, gp.y < 0.0f.xxx.y, gp.z < 0.0f.xxx.z));
    bool _137;
    if (!_129)
    {
        _137 = any(bool3(gp.x > gridMax.x, gp.y > gridMax.y, gp.z > gridMax.z));
    }
    else
    {
        _137 = _129;
    }
    if (_137)
    {
        sh[0] = float3(0.1063472330570220947265625f, 0.1063472330570220947265625f, 0.124071769416332244873046875f);
        sh[1] = 0.0f.xxx;
        sh[2] = 0.0f.xxx;
        sh[3] = 0.0f.xxx;
        cascade_out_l0[int3(tid)] = float4(sh[0], 1.0f);
        cascade_out_l1x[int3(tid)] = float4(sh[1], 1.0f);
        cascade_out_l1y[int3(tid)] = float4(sh[2], 1.0f);
        cascade_out_l1z[int3(tid)] = float4(sh[3], 1.0f);
        return;
    }
    float3 f = clamp(frac(gp), 0.0f.xxx, 1.0f.xxx);
    int3 b0 = clamp(int3(floor(gp)), int3(0, 0, 0), int3(cp_ddgi_counts.xyz) - int3(1, 1, 1));
    int3 b1 = min((b0 + int3(1, 1, 1)), (int3(cp_ddgi_counts.xyz) - int3(1, 1, 1)));
    float w[8];
    w[0] = ((1.0f - f.x) * (1.0f - f.y)) * (1.0f - f.z);
    w[1] = (f.x * (1.0f - f.y)) * (1.0f - f.z);
    w[2] = ((1.0f - f.x) * f.y) * (1.0f - f.z);
    w[3] = (f.x * f.y) * (1.0f - f.z);
    w[4] = ((1.0f - f.x) * (1.0f - f.y)) * f.z;
    w[5] = (f.x * (1.0f - f.y)) * f.z;
    w[6] = ((1.0f - f.x) * f.y) * f.z;
    w[7] = (f.x * f.y) * f.z;
    int3 pc[8];
    pc[0] = int3(b0.x, b0.y, b0.z);
    pc[1] = int3(b1.x, b0.y, b0.z);
    pc[2] = int3(b0.x, b1.y, b0.z);
    pc[3] = int3(b1.x, b1.y, b0.z);
    pc[4] = int3(b0.x, b0.y, b1.z);
    pc[5] = int3(b1.x, b0.y, b1.z);
    pc[6] = int3(b0.x, b1.y, b1.z);
    pc[7] = int3(b1.x, b1.y, b1.z);
    for (int c = 0; c < 4; c++)
    {
        sh[c] = 0.0f.xxx;
    }
    float wsum = 0.0f;
    for (int p = 0; p < 8; p++)
    {
        if (w[p] < 0.00999999977648258209228515625f)
        {
            continue;
        }
        uint pIdx = uint(pc[p].x) + (uint(cp_ddgi_counts.x) * (uint(pc[p].y) + (uint(cp_ddgi_counts.y) * uint(pc[p].z))));
        uint base = pIdx * 9u;
        for (int c_1 = 0; c_1 < 4; c_1++)
        {
            sh[c_1] += (asfloat(ddgiIrr.Load4((base + uint(c_1)) * 16 + 0)).xyz * w[p]);
        }
        wsum += w[p];
    }
    if (wsum > 0.0f)
    {
        for (int c_2 = 0; c_2 < 4; c_2++)
        {
            sh[c_2] /= wsum.xxx;
        }
    }
    float _498;
    if (cp_frame_index >= 2.0f)
    {
        _498 = cp_temporal_weight;
    }
    else
    {
        _498 = 0.0f;
    }
    float tw = _498;
    float3 prevL0 = cascade_prev_l0.Load(int4(int3(tid), 0)).xyz;
    float3 prevL1x = cascade_prev_l1x.Load(int4(int3(tid), 0)).xyz;
    float3 prevL1y = cascade_prev_l1y.Load(int4(int3(tid), 0)).xyz;
    float3 prevL1z = cascade_prev_l1z.Load(int4(int3(tid), 0)).xyz;
    sh[0] = lerp(sh[0], prevL0, tw.xxx);
    sh[1] = lerp(sh[1], prevL1x, tw.xxx);
    sh[2] = lerp(sh[2], prevL1y, tw.xxx);
    sh[3] = lerp(sh[3], prevL1z, tw.xxx);
    cascade_out_l0[int3(tid)] = float4(sh[0], 1.0f);
    cascade_out_l1x[int3(tid)] = float4(sh[1], 1.0f);
    cascade_out_l1y[int3(tid)] = float4(sh[2], 1.0f);
    cascade_out_l1z[int3(tid)] = float4(sh[3], 1.0f);
}

[numthreads(4, 4, 4)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
