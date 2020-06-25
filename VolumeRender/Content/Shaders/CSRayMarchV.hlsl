//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define _LIGHT_PASS_

#include "RayMarch.hlsli"

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------
Texture3D<float3> g_txLightMap;

//--------------------------------------------------------------------------------------
// Get light
//--------------------------------------------------------------------------------------
float3 GetLight(float3 pos, float3 step)
{
	const float3 tex = (pos + step) * 0.5 + 0.5;
	
	return g_txLightMap.SampleLevel(g_smpLinear, tex, 0.0);
}

#include "CSRayMarch.hlsl"
