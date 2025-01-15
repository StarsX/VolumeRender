//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define _HAS_DEPTH_MAP_
#define _HAS_SHADOW_MAP_
#define _HAS_LIGHT_PROBE_

#define	INF		asfloat(0x7f800000)
#define	FLT_MAX	3.402823466e+38

//--------------------------------------------------------------------------------------
// Constant buffers
//--------------------------------------------------------------------------------------
cbuffer cbPerObject
{
	float4x4 g_worldViewProjI;
	float4x4 g_worldViewProj;
	float4x3 g_worldI;
	float4x3 g_world;
};

cbuffer cbPerFrame
{
	float3 g_eyePt;
	float4x4 g_shadowViewProj;
	float3 g_lightPt;
	float4 g_lightColor;
	float4 g_ambient;
};

//--------------------------------------------------------------------------------------
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerState g_smpLinear;
