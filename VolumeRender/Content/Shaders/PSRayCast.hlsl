//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "RayCast.hlsli"

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------
Texture2D<float4> g_txHalvedCube[3];

//--------------------------------------------------------------------------------------
// Screen space to loacal space
//--------------------------------------------------------------------------------------
float3 TexcoordToLocalPos(float2 tex)
{
	float4 pos;
	pos.xy = tex * 2.0 - 1.0;
	pos.zw = float2(0.0, 1.0);
	pos.y = -pos.y;
	pos = mul(pos, g_worldViewProjI);
	
	return pos.xyz / pos.w;
}

//--------------------------------------------------------------------------------------
// Compute start point of the ray
//--------------------------------------------------------------------------------------
uint ComputeStartPoint(inout float3 pos, float3 rayDir)
{
	float3 one = -sign(rayDir);
	rayDir = all(abs(pos) <= 1.0) ? -rayDir : rayDir;

	//float U = asfloat(0x7f800000);	// INF
	float U = 3.402823466e+38;			// FLT_MAX
	uint hitSlice = 0xffffffff;

	[unroll]
	for (uint i = 0; i < 3; ++i)
	{
		const float u = (one[i] - pos[i]) / rayDir[i];
		if (u < 0.0h) continue;

		const uint j = (i + 1) % 3, k = (i + 2) % 3;
		if (abs(rayDir[j] * u + pos[j]) > 1.0) continue;
		if (abs(rayDir[k] * u + pos[k]) > 1.0) continue;
		if (u < U)
		{
			U = u;
			hitSlice = i;
		}
	}

	pos = clamp(rayDir * U + pos, -1.0, 1.0);

	return hitSlice;
}

//--------------------------------------------------------------------------------------
// Compute texcoord
//--------------------------------------------------------------------------------------
float2 ComputeHalvedCubeTexcoord(float3 pos, uint hitSlice)
{
	float2 tex;
	switch (hitSlice)
	{
	case 0: // X
		tex.x = sign(pos.x) * pos.z;
		tex.y = pos.y;
		break;
	case 1: // Y
		tex.x = pos.x;
		tex.y = sign(pos.y) * pos.z;
		break;
	case 2: // Z
		tex.x = -sign(pos.z) * pos.x;
		tex.y = pos.y;
		break;
	default:
		tex = 0.0;
		break;
	}
	tex.xy = tex.xy * 0.5 + 0.5;
	tex.y = 1.0 - tex.y;

	return tex;
}

//--------------------------------------------------------------------------------------
// Store image data to the corresponding slice
//--------------------------------------------------------------------------------------
float4 SampleSlice(float2 tex, uint slice, Texture2D<float4> txHalvedCube[3])
{
	switch (slice)
	{
	case 0: // X
		return txHalvedCube[0].SampleLevel(g_smpLinear, tex, 0.0);
	case 1: // Y
		return txHalvedCube[1].SampleLevel(g_smpLinear, tex, 0.0);
	case 2: // Z
		return txHalvedCube[2].SampleLevel(g_smpLinear, tex, 0.0);
	default:
		return 0.0;
	}
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
min16float4 main(PSIn input) : SV_TARGET
{
	float3 pos = TexcoordToLocalPos(input.Tex);	// The point on the near plane
	const float3 localSpaceEyePt = mul(g_eyePos, g_worldI).xyz;
	const float3 rayDir = normalize(pos - localSpaceEyePt);
	pos = localSpaceEyePt;

	const uint hitSlice = ComputeStartPoint(pos, rayDir);
	if (hitSlice > 2) discard;

	const float2 tex = ComputeHalvedCubeTexcoord(pos, hitSlice);

	float4 result = SampleSlice(tex, hitSlice, g_txHalvedCube);
	//if (result.w < 0.0) discard;

	return min16float4(result.xyz, result.w);
}
