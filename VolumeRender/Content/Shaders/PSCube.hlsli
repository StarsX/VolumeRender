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

#if !_USE_PURE_ARRAY_
float3 ClampEdge(float3 pos, float3 rayDir, float bound)
{
	[unroll]
	for (uint i = 0; i < 3; ++i)
	{
		const float axis = pos[i];
		//if (abs(axis) > bound)
		//{
		//	float3 norm = 0.0;
		//	norm[i] = axis >= 0.0 ? 1.0 : -1.0;
		//	if (dot(norm, rayDir) < 0.0)
		//		pos[i] = axis >= 0.0 ? bound : -bound;
		//}
		if (abs(axis) > bound && axis * rayDir[i] < 0.0)
			pos[i] = axis >= 0.0 ? bound : -bound;
	}

	return pos;
}
#endif

//--------------------------------------------------------------------------------------
// Cube interior-surface casting
//--------------------------------------------------------------------------------------
min16float4 CubeCast(float3 uvw, float2 uv, uint2 idx)
{
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

	const min16float2 domain = min16float2(frac(uv + 0.5));
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
