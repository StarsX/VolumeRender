//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "RayCast.hlsli"

#define NUM_SAMPLES			256
#define NUM_LIGHT_SAMPLES	64
#define ABSORPTION			1.0
#define ZERO_THRESHOLD		0.01
#define ONE_THRESHOLD		0.99

//--------------------------------------------------------------------------------------
// Constant
//--------------------------------------------------------------------------------------
static const min16float g_maxDist = 2.0 * sqrt(3.0);

static const min16float3 g_light = min16float3(1.0, 0.7, 0.3);
static const min16float3 g_ambient = min16float3(0.0, 0.1, 0.3);

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------
Texture3D<float4> g_txGrid;

//--------------------------------------------------------------------------------------
// Sample density field
//--------------------------------------------------------------------------------------
min16float4 GetSample(float3 tex)
{
	return min16float4(g_txGrid.SampleLevel(g_smpLinear, tex, 0.0));
	//return min16float4(0.0, 0.5, 1.0, 0.5);
}

//--------------------------------------------------------------------------------------
// Get opacity
//--------------------------------------------------------------------------------------
min16float GetOpacity(min16float density, min16float stepScale)
{
	return saturate(density * stepScale * ABSORPTION * 4.0);
}
