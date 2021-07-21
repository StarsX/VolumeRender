//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cbPerObject
{
	matrix g_worldViewProjI;
	matrix g_worldViewProj;
	matrix g_shadowWVP;
	matrix g_worldI;
	matrix g_lightMapWorld;
	float4 g_eyePos;
	float4 g_lightPos;
	float4 g_lightColor;
	float4 g_ambient;
};

//--------------------------------------------------------------------------------------
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerState g_smpLinear;
