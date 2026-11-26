// Vulkan reference: PostProcess/MirrorComposite.frag (main).
cbuffer CompositeParams : register(b0, space0)
{
    row_major float4x4 view_projection : packoffset(c0);
    row_major float4x4 reflected_view_projection : packoffset(c4);
    float4 plane_position : packoffset(c8);
    float4 plane_normal : packoffset(c9);
    float4 plane_axes : packoffset(c10);
    float4 camera_position : packoffset(c11);
};

Texture2D<float4> reflectionTex : register(t1, space0);
SamplerState reflectionSampler : register(s2, space0);

struct StageInput
{
    float3 vWorldPos : TEXCOORD0;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float4 clip = mul(float4(input.vWorldPos, 1.0f), reflected_view_projection);
    float3 ndc = clip.xyz / clip.w.xxx;
    float2 uv = float2((ndc.x * 0.5f) + 0.5f, (ndc.y * 0.5f) + 0.5f);
    if (((((uv.x < 0.0f) || (uv.x > 1.0f)) || (uv.y < 0.0f)) || (uv.y > 1.0f)))
    {
        discard;
    }
    float3 reflection = reflectionTex.Sample(reflectionSampler, uv).xyz;
    float3 viewDir = normalize(input.vWorldPos - camera_position.xyz);
    float cosTheta = abs(dot(viewDir, normalize(plane_normal.xyz)));
    float fresnel = lerp(0.35f, 0.9f, pow(1.0f - cosTheta, 3.0f));
    float alpha = clamp(camera_position.w * fresnel, 0.0f, 1.0f);
    output.outColor = float4(reflection, alpha);
    return output;
}
