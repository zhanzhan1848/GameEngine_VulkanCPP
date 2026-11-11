struct StaticCompositeInfo
{
    uint4 meta0;
    uint4 meta1;
    uint4 meta2;
    float4 center_w;
    float4 mcol0;
    float4 mcol1;
    float4 mcol2;
    float4 local0;
    float4 local1;
};

static const uint3 gl_WorkGroupSize = uint3(256u, 1u, 1u);

ByteAddressBuffer ci : register(t3, space0);
RWByteAddressBuffer atlasTriangles : register(u6, space0);
ByteAddressBuffer pb : register(t4, space0);
RWTexture2D<float4> emissive_atlas : register(u5, space0);
RWTexture2D<float> depth_atlas : register(u2, space0);
RWTexture2D<unorm float4> albedo_atlas : register(u0, space0);
RWTexture2D<float2> normal_atlas : register(u1, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

uint2 spvImageSize(RWTexture2D<float> Tex, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(ret.x, ret.y);
    Param = 0u;
    return ret;
}

uint spvPackHalf2x16(float2 value)
{
    uint2 Packed = f32tof16(value);
    return Packed.x | (Packed.y << 16);
}

float2 spvUnpackHalf2x16(uint value)
{
    return f16tof32(uint2(value & 0xffff, value >> 16));
}

// Returns the determinant of a 2x2 matrix.
float spvDet2x2(float a1, float a2, float b1, float b2)
{
    return a1 * b2 - b1 * a2;
}

// Returns the inverse of a matrix, by using the algorithm of calculating the classical
// adjoint and dividing by the determinant. The contents of the matrix are changed.
float3x3 spvInverse(float3x3 m)
{
    float3x3 adj;	// The adjoint matrix (inverse after dividing by determinant)

    // Create the transpose of the cofactors, as the classical adjoint of the matrix.
    adj[0][0] =  spvDet2x2(m[1][1], m[1][2], m[2][1], m[2][2]);
    adj[0][1] = -spvDet2x2(m[0][1], m[0][2], m[2][1], m[2][2]);
    adj[0][2] =  spvDet2x2(m[0][1], m[0][2], m[1][1], m[1][2]);

    adj[1][0] = -spvDet2x2(m[1][0], m[1][2], m[2][0], m[2][2]);
    adj[1][1] =  spvDet2x2(m[0][0], m[0][2], m[2][0], m[2][2]);
    adj[1][2] = -spvDet2x2(m[0][0], m[0][2], m[1][0], m[1][2]);

    adj[2][0] =  spvDet2x2(m[1][0], m[1][1], m[2][0], m[2][1]);
    adj[2][1] = -spvDet2x2(m[0][0], m[0][1], m[2][0], m[2][1]);
    adj[2][2] =  spvDet2x2(m[0][0], m[0][1], m[1][0], m[1][1]);

    // Calculate the determinant as a combination of the cofactors of the first row.
    float det = (adj[0][0] * m[0][0]) + (adj[0][1] * m[1][0]) + (adj[0][2] * m[2][0]);

    // Divide the classical adjoint matrix by the determinant.
    // If determinant is zero, matrix is not invertable, so leave it unchanged.
    return (det != 0.0f) ? (adj * (1.0f / det)) : m;
}

float3 octDecode(inout float2 f)
{
    f = (f * 2.0f) - 1.0f.xx;
    float3 n = float3(f.x, f.y, (1.0f - abs(f.x)) - abs(f.y));
    float t = clamp(-n.z, 0.0f, 1.0f);
    float3 _108 = n;
    float3 _112 = n;
    float2 _114 = _112.xy + lerp(t.xx, (-t).xx, step(0.0f.xx, _108.xy));
    n.x = _114.x;
    n.y = _114.y;
    return normalize(n);
}

float2 octEncode(float3 n)
{
    float l1norm = (abs(n.x) + abs(n.y)) + abs(n.z);
    float2 result = n.xy / l1norm.xx;
    if (n.z < 0.0f)
    {
        result = (1.0f.xx - abs(result.yx)) * lerp((-1.0f).xx, 1.0f.xx, step(0.0f.xx, result));
    }
    return (result * 0.5f) + 0.5f.xx;
}

float3 mat3_from_cols(float4 c0, float4 c1, float4 c2, float3 v)
{
    return ((c0.xyz * v.x) + (c1.xyz * v.y)) + (c2.xyz * v.z);
}

void comp_main()
{
    uint tid = gl_GlobalInvocationID.x;
    uint lo = 0u;
    uint hi = ci.Load(20);
    bool found = false;
    StaticCompositeInfo m;
    while (lo < hi)
    {
        uint mid = (lo + hi) >> 1u;
        StaticCompositeInfo _186;
        _186.meta0 = ci.Load4(mid * 144 + 0);
        _186.meta1 = ci.Load4(mid * 144 + 16);
        _186.meta2 = ci.Load4(mid * 144 + 32);
        _186.center_w = asfloat(ci.Load4(mid * 144 + 48));
        _186.mcol0 = asfloat(ci.Load4(mid * 144 + 64));
        _186.mcol1 = asfloat(ci.Load4(mid * 144 + 80));
        _186.mcol2 = asfloat(ci.Load4(mid * 144 + 96));
        _186.local0 = asfloat(ci.Load4(mid * 144 + 112));
        _186.local1 = asfloat(ci.Load4(mid * 144 + 128));
        m.meta0 = _186.meta0;
        m.meta1 = _186.meta1;
        m.meta2 = _186.meta2;
        m.center_w = _186.center_w;
        m.mcol0 = _186.mcol0;
        m.mcol1 = _186.mcol1;
        m.mcol2 = _186.mcol2;
        m.local0 = _186.local0;
        m.local1 = _186.local1;
        if ((m.meta1.x + (m.meta0.z * m.meta0.z)) <= tid)
        {
            lo = mid + 1u;
        }
        else
        {
            hi = mid;
        }
    }
    StaticCompositeInfo info;
    if (lo < ci.Load(20))
    {
        StaticCompositeInfo _238;
        _238.meta0 = ci.Load4(lo * 144 + 0);
        _238.meta1 = ci.Load4(lo * 144 + 16);
        _238.meta2 = ci.Load4(lo * 144 + 32);
        _238.center_w = asfloat(ci.Load4(lo * 144 + 48));
        _238.mcol0 = asfloat(ci.Load4(lo * 144 + 64));
        _238.mcol1 = asfloat(ci.Load4(lo * 144 + 80));
        _238.mcol2 = asfloat(ci.Load4(lo * 144 + 96));
        _238.local0 = asfloat(ci.Load4(lo * 144 + 112));
        _238.local1 = asfloat(ci.Load4(lo * 144 + 128));
        info.meta0 = _238.meta0;
        info.meta1 = _238.meta1;
        info.meta2 = _238.meta2;
        info.center_w = _238.center_w;
        info.mcol0 = _238.mcol0;
        info.mcol1 = _238.mcol1;
        info.mcol2 = _238.mcol2;
        info.local0 = _238.local0;
        info.local1 = _238.local1;
        bool _260 = tid >= info.meta1.x;
        bool _273;
        if (_260)
        {
            _273 = tid < (info.meta1.x + (info.meta0.z * info.meta0.z));
        }
        else
        {
            _273 = _260;
        }
        found = _273;
    }
    if (!found)
    {
        return;
    }
    uint res = info.meta0.z;
    uint page_res = info.meta0.w;
    uint local_texel = tid - info.meta1.x;
    uint lx = local_texel % res;
    uint ly = local_texel / res;
    int2 atlas_uv = int2(int(info.meta0.x + lx), int(info.meta0.y + ly));
    emissive_atlas[atlas_uv] = 0.0f.xxxx;
    uint _328_dummy_parameter;
    uint identityAt = (uint(atlas_uv.y) * uint(int2(spvImageSize(depth_atlas, _328_dummy_parameter)).x)) + uint(atlas_uv.x);
    atlasTriangles.Store(identityAt * 4 + 0, 4294967295u);
    float u = (float(lx) + 0.5f) / float(res);
    float v = (float(ly) + 0.5f) / float(res);
    uint axis = info.meta1.z & 255u;
    uint dir = (info.meta1.z >> 8u) & 255u;
    float3 e_w;
    e_w.x = ((abs(info.mcol0.x) * info.local0.w) + (abs(info.mcol1.x) * info.local1.x)) + (abs(info.mcol2.x) * info.local1.y);
    e_w.y = ((abs(info.mcol0.y) * info.local0.w) + (abs(info.mcol1.y) * info.local1.x)) + (abs(info.mcol2.y) * info.local1.y);
    e_w.z = ((abs(info.mcol0.z) * info.local0.w) + (abs(info.mcol1.z) * info.local1.x)) + (abs(info.mcol2.z) * info.local1.y);
    float3 q_w = info.center_w.xyz;
    if (axis == 0u)
    {
        q_w += float3(0.0f, ((u - 0.5f) * e_w.y) * 2.0f, ((v - 0.5f) * e_w.z) * 2.0f);
    }
    else
    {
        if (axis == 1u)
        {
            q_w += float3(((u - 0.5f) * e_w.x) * 2.0f, 0.0f, ((v - 0.5f) * e_w.z) * 2.0f);
        }
        else
        {
            q_w += float3(((u - 0.5f) * e_w.x) * 2.0f, ((v - 0.5f) * e_w.y) * 2.0f, 0.0f);
        }
    }
    float3 dl = mul(q_w - info.center_w.xyz, spvInverse(float3x3(float3(info.mcol0.xyz), float3(info.mcol1.xyz), float3(info.mcol2.xyz))));
    uint local_axis = info.meta1.w & 255u;
    uint local_dir = (info.meta1.w >> 8u) & 255u;
    float3 extent_l = float3(info.local0.w, info.local1.x, info.local1.y);
    float eu = extent_l[uint(local_axis == 0u)];
    float ev = extent_l[(local_axis == 2u) ? 1u : 2u];
    float u_l;
    float v_l;
    if (axis == 0u)
    {
        u_l = (dl.y / (eu * 2.0f)) + 0.5f;
        v_l = (dl.z / (ev * 2.0f)) + 0.5f;
    }
    else
    {
        if (axis == 1u)
        {
            u_l = (dl.x / (eu * 2.0f)) + 0.5f;
            v_l = (dl.z / (ev * 2.0f)) + 0.5f;
        }
        else
        {
            u_l = (dl.x / (eu * 2.0f)) + 0.5f;
            v_l = (dl.y / (ev * 2.0f)) + 0.5f;
        }
    }
    int px = int(floor(u_l * float(page_res)));
    int py = int(floor(v_l * float(page_res)));
    bool _617 = (px < 0) || (py < 0);
    bool _625;
    if (!_617)
    {
        _625 = px >= int(page_res);
    }
    else
    {
        _625 = _617;
    }
    bool _633;
    if (!_625)
    {
        _633 = py >= int(page_res);
    }
    else
    {
        _633 = _625;
    }
    if (_633)
    {
        albedo_atlas[atlas_uv] = 0.0f.xxxx;
        normal_atlas[atlas_uv] = float4(0.5f, 0.5f, 0.0f, 1.0f).xy;
        depth_atlas[atlas_uv] = float4(-1.0f, 0.0f, 0.0f, 1.0f).x;
        return;
    }
    uint ptex = (uint(py) * page_res) + uint(px);
    uint base = info.meta2.x + ptex;
    uint albedo_word = pb.Load(base * 4 + 0);
    float coverage = float((albedo_word >> 24u) & 255u) / 255.0f;
    if (coverage < 0.5f)
    {
        albedo_atlas[atlas_uv] = 0.0f.xxxx;
        normal_atlas[atlas_uv] = float4(0.5f, 0.5f, 0.0f, 1.0f).xy;
        depth_atlas[atlas_uv] = float4(-1.0f, 0.0f, 0.0f, 1.0f).x;
        return;
    }
    atlasTriangles.Store(identityAt * 4 + 0, pb.Load((info.meta2.w + ptex) * 4 + 0));
    emissive_atlas[atlas_uv] = float4(info.local0.xyz, 1.0f);
    float r = float(albedo_word & 255u) / 255.0f;
    float g = float((albedo_word >> 8u) & 255u) / 255.0f;
    float b = float((albedo_word >> 16u) & 255u) / 255.0f;
    albedo_atlas[atlas_uv] = float4(r, g, b, 0.501960813999176025390625f);
    float2 e = spvUnpackHalf2x16(pb.Load((info.meta2.y + ptex) * 4 + 0));
    float2 param = e;
    float3 _744 = octDecode(param);
    float3 n_l = _744;
    float3 n_w = normalize(mul(n_l, transpose(spvInverse(float3x3(float3(info.mcol0.xyz), float3(info.mcol1.xyz), float3(info.mcol2.xyz))))));
    float3 _776;
    if (axis == 0u)
    {
        _776 = float3((dir > 0u) ? 1.0f : (-1.0f), 0.0f, 0.0f);
    }
    else
    {
        float3 _786;
        if (axis == 1u)
        {
            _786 = float3(0.0f, (dir > 0u) ? 1.0f : (-1.0f), 0.0f);
        }
        else
        {
            _786 = float3(0.0f, 0.0f, (dir > 0u) ? 1.0f : (-1.0f));
        }
        _776 = _786;
    }
    float3 f_w = _776;
    if (dot(n_w, f_w) > 0.0f)
    {
        n_w = -n_w;
    }
    float3 param_1 = n_w;
    normal_atlas[atlas_uv] = float4(octEncode(param_1), 0.0f, 1.0f).xy;
    float d_l = asfloat(pb.Load((info.meta2.z + ptex) * 4 + 0));
    float3 f_l = 0.0f.xxx;
    f_l[local_axis] = (local_dir > 0u) ? 1.0f : (-1.0f);
    float3 off_l;
    if (local_axis == 0u)
    {
        off_l = float3(f_l.x * d_l, ((u_l - 0.5f) * eu) * 2.0f, ((v_l - 0.5f) * ev) * 2.0f);
    }
    else
    {
        if (local_axis == 1u)
        {
            off_l = float3(((u_l - 0.5f) * eu) * 2.0f, f_l.y * d_l, ((v_l - 0.5f) * ev) * 2.0f);
        }
        else
        {
            off_l = float3(((u_l - 0.5f) * eu) * 2.0f, ((v_l - 0.5f) * ev) * 2.0f, f_l.z * d_l);
        }
    }
    float4 param_2 = info.mcol0;
    float4 param_3 = info.mcol1;
    float4 param_4 = info.mcol2;
    float3 param_5 = off_l;
    float3 p_w = info.center_w.xyz + mat3_from_cols(param_2, param_3, param_4, param_5);
    float d_w = dot(p_w - info.center_w.xyz, f_w);
    depth_atlas[atlas_uv] = float4(d_w, 0.0f, 0.0f, 1.0f).x;
}

[numthreads(256, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
