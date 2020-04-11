//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "WeightedAlpha.hlsli"

#define a output.Alpha

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
	float Alpha : SV_TARGET1;
};

//--------------------------------------------------------------------------------------
// Constants
//--------------------------------------------------------------------------------------
cbuffer cbPerObject
{
	float g_particleSize;
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

	const float r_sq = dot(input.Domain, input.Domain);
	a = 1.0 - r_sq;
	clip(a);

	const float3 light = g_txLight.SampleLevel(g_smpLinear, input.Tex.xyz, 0.0);

	a *= a * 0.5;
	a *= 0.125;
	output.Color.w = a;
	output.Color.w *= DepthWeight0(input.Tex.w, g_particleSize);
	//output.Color.w *= DepthWeight1(input.Pos.w);
	output.Color.w *= DepthWeight2(input.Pos.w);
	//output.Color.w *= DepthWeight3(input.Pos.w);
	//output.Color.w *= DepthWeight4(input.Pos.z);

	output.Color.xyz = input.Color * light;

	return output;
}
