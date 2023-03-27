#include "Fractals.hlsli"

#define SAMPLES 4

struct ShaderConstants
{
	float Width;
	float Height;
	uint frame;
};

ConstantBuffer<ShaderConstants>						ShaderParams				: register(b1);

float4 FillColorPS(in noperspective float4 Position : SV_Position,
	in noperspective float2 UV : TEXCOORD) : SV_Target0
{
	const float offset = 0.f;
const float2 offsets[4] =
{
	float2(-offset, offset),
	float2(offset, offset),
	float2(offset, -offset),
	float2(-offset, -offset),
};
	const float2 invDim = float2(1.f / ShaderParams.Width, 1.f / ShaderParams.Height);
	float3 color = 0.f;
	for (int i = 0; i < SAMPLES; i++)
	{
		const float2 uv = (Position.xy + offsets[i]) * invDim;
		//float3 color = DrawMandelbrot(uv);
		color += DrawJuliaSet(uv, ShaderParams.frame);
	}
	
	return float4(float3(color.z, color.x, 1.f) * color.x / SAMPLES, 1.f);
	//return float4(color / SAMPLES, 1.f);
}

//noperspective