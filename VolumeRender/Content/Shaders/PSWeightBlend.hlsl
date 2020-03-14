//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "WeightedAlpha.hlsli"

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4 Pos : SV_POSITION;
	float4 Tex : TEXCOORD;
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
	float a = 1.0 - r_sq;
	clip(a);

	const float3 light = g_txLight.SampleLevel(g_smpLinear, input.Tex.xyz, 0.0);

	a *= a * 0.25;
	//output.Color.w = a * DepthWeight0(input.Tex.w);
	//output.Color.w = a * DepthWeight1(input.Pos.w);
	output.Color.w = a * DepthWeight2(input.Pos.w);
	//output.Color.w = a * DepthWeight3(input.Pos.w);
	//output.Color.w = a * DepthWeight4(input.Pos.z);

	output.Color.xyz = input.Color * light;
	output.Color.xyz *= output.Color.w;
	output.Transm = 1.0 - output.Color.w;

	return output;
}
