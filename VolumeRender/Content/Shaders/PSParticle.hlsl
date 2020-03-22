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

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------
Texture3D<float3> g_txLightMap;

//--------------------------------------------------------------------------------------
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerState g_smpLinear;

float4 main(PSIn input) : SV_TARGET
{
	const float r_sq = dot(input.Domain, input.Domain);
	float a = 1.0 - r_sq;
	clip(a);

	const float3 light = g_txLightMap.SampleLevel(g_smpLinear, input.Tex.xyz, 0.0);

	a *= a * 0.5;
	//a *= DepthWeight0(input.Tex.w);
	//a *= DepthWeight1(input.Pos.w);
	//a *= DepthWeight2(input.Pos.w);
	//a *= DepthWeight3(input.Pos.w);
	//a *= DepthWeight4(input.Pos.z);
	const float range = saturate((input.Pos.z - 1.5) / -2.5);
	const float rangeInv = 1.0 - range;
	a *= range * rangeInv * rangeInv;

	const float3 color = input.Color * light;
	
	return float4(color, saturate(a));
}
