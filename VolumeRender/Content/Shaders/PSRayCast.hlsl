//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "RayCast.hlsli"

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4 Pos	: SV_POSITION;
	float2 UV	: TEXCOORD;
};

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
// Screen space to local space
//--------------------------------------------------------------------------------------
float3 TexcoordToLocalPos(float2 uv)
{
	float4 pos;
	pos.xy = uv * 2.0 - 1.0;
	pos.zw = float2(0.0, 1.0);
	pos.y = -pos.y;
	pos = mul(pos, g_worldViewProjI);
	
	return pos.xyz / pos.w;
}

//--------------------------------------------------------------------------------------
// Compute end point of the ray on the cube surface
//--------------------------------------------------------------------------------------
uint ComputeRayHit(inout float3 pos, float3 rayDir)
{
	//float U = asfloat(0x7f800000);	// INF
	float U = 3.402823466e+38;			// FLT_MAX
	uint hitPlane = 0xffffffff;

	[unroll]
	for (uint i = 0; i < 3; ++i)
	{
		const float u = (sign(rayDir[i]) - pos[i]) / rayDir[i];
		if (u < 0.0) continue;

		const uint j = (i + 1) % 3, k = (i + 2) % 3;
		if (abs(rayDir[j] * u + pos[j]) > 1.0) continue;
		if (abs(rayDir[k] * u + pos[k]) > 1.0) continue;
		if (u < U)
		{
			U = u;
			hitPlane = i;
		}
	}

	//pos = clamp(rayDir * U + pos, -1.0, 1.0);
	pos = rayDir * U + pos;

	return hitPlane;
}

#if _USE_PURE_ARRAY_
//--------------------------------------------------------------------------------------
// Compute texcoord
//--------------------------------------------------------------------------------------
float3 ComputeCubeTexcoord(float3 pos, uint hitPlane)
{
	float3 tex;

	switch (hitPlane)
	{
	case 0: // X
		tex.x = -pos.x * pos.z;
		tex.y = pos.y;
		tex.z = pos.x < 0.0 ? hitPlane * 2 + 1 : hitPlane * 2;
		break;
	case 1: // Y
		tex.x = pos.x;
		tex.y = -pos.y * pos.z;
		tex.z = pos.y < 0.0 ? hitPlane * 2 + 1 : hitPlane * 2;
		break;
	case 2: // Z
		tex.x = pos.z * pos.x;
		tex.y = pos.y;
		tex.z = pos.z < 0.0 ? hitPlane * 2 + 1 : hitPlane * 2;
		break;
	default:
		tex = 0.0;
		break;
	}
	tex.xy = tex.xy * 0.5 + 0.5;
	tex.y = 1.0 - tex.y;

	return tex;
}
#else
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
// Pixel Shader
//--------------------------------------------------------------------------------------
min16float4 main(PSIn input) : SV_TARGET
{
	float3 pos = TexcoordToLocalPos(input.UV);	// The point on the near plane
	const float3 localSpaceEyePt = mul(g_eyePos, g_worldI).xyz;
	const float3 rayDir = normalize(pos - localSpaceEyePt);

	const uint hitPlane = ComputeRayHit(pos, rayDir);
	if (hitPlane > 2) discard;

#if _USE_PURE_ARRAY_
	const float3 uvw = ComputeCubeTexcoord(pos, hitPlane);
#else
	float2 gridSize;
	g_txCubeMap.GetDimensions(gridSize.x, gridSize.y);
	const float3 uvw = ClampEdge(pos, rayDir, 1.0 - 1.0 / gridSize.x);
#endif

#if 0
	float4 result = g_txCubeMap.SampleLevel(g_smpLinear, uvw, 0.0);
#else
	const float depth = g_txDepth[input.Pos.xy];
	const float4 r = g_txCubeMap.GatherRed(g_smpLinear, uvw);
	const float4 g = g_txCubeMap.GatherGreen(g_smpLinear, uvw);
	const float4 b = g_txCubeMap.GatherBlue(g_smpLinear, uvw);
	const float4 a = g_txCubeMap.GatherAlpha(g_smpLinear, uvw);
	const float4 z = g_txCubeDepth.GatherRed(g_smpLinear, uvw);

	min16float4 results[4];
	[unroll]
	for (uint i = 0; i < 4; ++i) results[i] = min16float4(r[i], g[i], b[i], a[i]);

	min16float4 result = 0.0;
	min16float ws = 0.0;
	[unroll]
	//for (i = 0; i < 4; ++i) result = results[i].w > result.w ? results[i] : result;
	for (i = 0; i < 4; ++i)
	{
		const min16float w = abs(depth - z[i]) < 0.01;
		result += results[i] * w;
		ws += w;
	}
	result /= ws;
#endif
	
	//if (result.w <= 0.0) discard;

	return min16float4(result.xyz, result.w);
}
