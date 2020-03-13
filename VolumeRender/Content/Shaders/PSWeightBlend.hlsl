//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4 Pos : SV_POSITION;
	float3 Tex : TEXCOORD;
	float3 Color : COLOR;
	float2 Domain : DOMAIN;
};

struct PSOut
{
	float4 Color : SV_TARGET;
	float Transm : SV_TARGET1;
};

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------
Texture3D<float3> g_txLight;

//--------------------------------------------------------------------------------------
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerState g_smpLinear;

PSOut main(PSIn input)
{
	PSOut output;

	const float2 disp = input.Domain * 2.0 - 1.0;
	const float r_sq = dot(disp, disp);
	const float a = 1.0 - r_sq;
	clip(a);

	const float3 light = g_txLight.SampleLevel(g_smpLinear, input.Tex, 0.0);

	output.Color.xyz = input.Color * light;
	output.Color.w = a * a * 0.125;
	output.Color.xyz *= output.Color.w;
	output.Transm = 1.0 - output.Color.w;

	return output;
}
