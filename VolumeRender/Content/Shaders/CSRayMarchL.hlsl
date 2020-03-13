//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "RayMarch.hlsli"

//--------------------------------------------------------------------------------------
// Constant
//--------------------------------------------------------------------------------------
static const min16float g_stepScale = g_maxDist / NUM_LIGHT_SAMPLES;

//--------------------------------------------------------------------------------------
// Unordered access texture
//--------------------------------------------------------------------------------------
RWTexture3D<float3> g_rwLightMap;

//--------------------------------------------------------------------------------------
// Compute Shader
//--------------------------------------------------------------------------------------
[numthreads(4, 4, 4)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	float3 gridSize;
	g_rwLightMap.GetDimensions(gridSize.x, gridSize.y, gridSize.z);

	float3 pos = (DTid + 0.5) / gridSize * 2.0 - 1.0;
	pos.y = -pos.y;
#ifdef _POINT_LIGHT_
	const float3 step = normalize(g_localSpaceLightPt - pos) * g_stepScale;
#else
	const float3 step = normalize(g_localSpaceLightPt) * g_stepScale;
#endif

	// Transmittance
	min16float shadow = 1.0;

	for (uint i = 0; i < NUM_LIGHT_SAMPLES; ++i)
	{
		if (any(abs(pos) > 1.0)) break;
		const float3 tex = pos * min16float3(0.5, -0.5, 0.5) + 0.5;

		// Get a sample along light ray
		const min16float density = GetSample(tex).w;

		// Attenuate ray-throughput along light direction
		shadow *= 1.0 - GetOpacity(density, g_stepScale);
		if (shadow < ZERO_THRESHOLD) break;

		// Update position along light ray
		pos += step;
	}

	g_rwLightMap[DTid] = g_light * shadow + g_ambient;
}
