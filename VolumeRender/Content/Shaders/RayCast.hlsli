//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define _HAS_DEPTH_MAP_
#define _HAS_SHADOW_MAP_

#define DENSITY_SCALE 1.0

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cbPerObject
{
	matrix g_worldViewProjI;
	matrix g_worldViewProj;
	matrix g_shadowWVP;
	float4x3 g_worldI;
	float4x3 g_lightMapWorld;
	float4 g_eyePos;
	float4 g_lightPos;
	float4 g_lightColor;
	float4 g_ambient;
};

//--------------------------------------------------------------------------------------
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerState g_smpLinear;
