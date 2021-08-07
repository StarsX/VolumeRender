//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define _HAS_DEPTH_MAP_
#define _HAS_SHADOW_MAP_

#define	INF				asfloat(0x7f800000)
#define	FLT_MAX			3.402823466e+38
#define DENSITY_SCALE	1.0

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cbPerObject
{
	matrix g_worldViewProjI;
	matrix g_worldViewProj;
	matrix g_shadowWVP;
	float4x3 g_worldI;
	float4x4 g_localToLight;
};

cbuffer cbPerFrame
{
	float3 g_eyePt;
	float4x3 g_lightMapWorld;
	float4 g_lightPos;
	float4 g_lightColor;
	float4 g_ambient;
};

//--------------------------------------------------------------------------------------
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerState g_smpLinear;
