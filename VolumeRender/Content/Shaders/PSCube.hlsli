//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "RayCast.hlsli"

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------
#if _USE_PURE_ARRAY_
Texture2DArray<float4> g_txCubeMap;
Texture2DArray<float> g_txCubeDepth;
#else
TextureCube<float4> g_txCubeMap;
TextureCube<float4> g_txCubeDepth;
#endif
Texture2D<float> g_txDepth;

//--------------------------------------------------------------------------------------
// Get domain location
//--------------------------------------------------------------------------------------
min16float2 GetDomain(float2 uv, float3 pos, float3 rayDir, float2 gridSize)
{
	uv *= gridSize;
	min16float2 domain = min16float2(frac(uv + 0.5));

#if !_USE_PURE_ARRAY_
	const float bound = gridSize.x - 1.0;
	const float3 axes = pos * gridSize.x;
	if (any(abs(axes) > bound && axes * rayDir < 0.0))
	{
		// Need to clamp edge
		uv = min(uv, gridSize - 0.5);
		domain = uv.x < 0.5 ? 1.0 : 0.0;
	}
#endif

	return domain;
}

//--------------------------------------------------------------------------------------
// Cube interior-surface casting
//--------------------------------------------------------------------------------------
min16float4 CubeCast(uint2 idx, float3 uvw, float3 pos, float3 rayDir)
{
	float2 gridSize;
	g_txCubeMap.GetDimensions(gridSize.x, gridSize.y);
	float2 uv = uvw.xy;

#if !_USE_PURE_ARRAY_
	uvw = pos;
#endif

#if 0
	float4 result = g_txCubeMap.SampleLevel(g_smpLinear, uvw, 0.0);
#else
	const float depth = g_txDepth[idx];
	const float4 r = g_txCubeMap.GatherRed(g_smpLinear, uvw);
	const float4 g = g_txCubeMap.GatherGreen(g_smpLinear, uvw);
	const float4 b = g_txCubeMap.GatherBlue(g_smpLinear, uvw);
	const float4 a = g_txCubeMap.GatherAlpha(g_smpLinear, uvw);
	const float4 z = g_txCubeDepth.GatherRed(g_smpLinear, uvw);

	min16float4 results[4];
	[unroll]
	for (uint i = 0; i < 4; ++i) results[i] = min16float4(r[i], g[i], b[i], a[i]);

	const min16float2 domain = GetDomain(uv, pos, rayDir, gridSize);
	const min16float2 domainInv = 1.0 - domain;
	const min16float4 wb =
	{
		domainInv.x * domain.y,
		domain.x * domain.y,
		domain.x * domainInv.y,
		domainInv.x * domainInv.y
	};

	min16float4 result = 0.0;
	min16float ws = 0.0;
	[unroll]
	for (i = 0; i < 4; ++i)
	{
		min16float w = abs(depth - z[i]) < 0.01;
		w *= wb[i];

		result += results[i] * w;
		ws += w;
	}

	result /= ws;
#endif

	if (result.w <= 0.0) discard;

	return min16float4(result.xyz, result.w);
}
