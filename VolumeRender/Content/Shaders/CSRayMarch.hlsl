//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "RayMarch.hlsli"

//--------------------------------------------------------------------------------------
// Unordered access textures
//--------------------------------------------------------------------------------------
RWTexture2DArray<float4> g_rwCubeMap;
RWTexture2DArray<float> g_rwCubeDepth;

//--------------------------------------------------------------------------------------
// Get the local-space position of the grid surface
//--------------------------------------------------------------------------------------
float3 GetLocalPos(float2 pos, uint slice, RWTexture2DArray<float4> rwCubeMap)
{
	float3 gridSize;
	rwCubeMap.GetDimensions(gridSize.x, gridSize.y, gridSize.z);
	
	pos = (pos + 0.5) / gridSize.xy * 2.0 - 1.0;
	pos.y = -pos.y;

	switch (slice)
	{
	case 0: // +X
		return float3(1.0, pos.y, -pos.x);
	case 1: // -X
		return float3(-1.0, pos.y, pos.x);
	case 2: // +Y
		return float3(pos.x, 1.0, -pos.y);
	case 3: // -Y
		return float3(pos.x, -1.0, pos.y);
	case 4: // +Z
		return float3(pos.x, pos.y, 1.0);
	case 5: // -Z
		return float3(-pos.x, pos.y, -1.0);
	default:
		return 0.0;
	}
}

//--------------------------------------------------------------------------------------
// Check the visibility of the slice
//--------------------------------------------------------------------------------------
bool IsVisible(uint slice, float3 target, float3 localSpaceEyePt)
{
	const uint plane = slice >> 1;
	const float viewComp = localSpaceEyePt[plane] - target[plane];

	return (slice & 0x1) ? viewComp > 0.0 : viewComp < 0.0;
}

//--------------------------------------------------------------------------------------
// Get clip-space position
//--------------------------------------------------------------------------------------
float3 GetClipPos(float3 rayOrigin, float3 rayDir)
{
	float4 hPos = float4(rayOrigin + 0.01 * rayDir, 1.0);
	hPos = mul(hPos, g_worldViewProj);

	const float2 xy = hPos.xy / hPos.w;
	float2 uv = xy * 0.5 + 0.5;
	uv.y = 1.0 - uv.y;

	const float4 depths = g_txDepth.Gather(g_smpLinear, uv);
	const float2 zs = max(depths.xy, depths.zw);
	const float z = max(zs.x, zs.y);

	return float3(xy, z);
}

//--------------------------------------------------------------------------------------
// Compute Shader
//--------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	float3 rayOrigin = mul(g_eyePos, g_worldI);
	//if (rayOrigin[DTid.z >> 1] == 0.0) return;

	const float3 target = GetLocalPos(DTid.xy, DTid.z, g_rwCubeMap);
	if (!IsVisible(DTid.z, target, rayOrigin)) return;

	const float3 rayDir = normalize(target - rayOrigin);
	if (!ComputeRayOrigin(rayOrigin, rayDir)) return;

	// Calculate occluded end point
	const float3 pos = GetClipPos(rayOrigin, rayDir);
	g_rwCubeDepth[DTid] = pos.z;
	const float tMax = GetTMax(pos, rayOrigin, rayDir);
	
#ifdef _POINT_LIGHT_
	const float3 localSpaceLightPt = mul(g_lightPos, g_worldI);
#else
	const float3 localSpaceLightPt = mul(g_lightPos.xyz, (float3x3)g_worldI);
	const float3 lightStep = normalize(localSpaceLightPt) * g_lightStepScale;
#endif

	// Transmittance
	min16float transm = 1.0;

	// In-scattered radiance
	min16float3 scatter = 0.0;

	float t = 0.0;
	for (uint i = 0; i < NUM_SAMPLES; ++i)
	{
		const float3 pos = rayOrigin + rayDir * t;
		if (any(abs(pos) > 1.0)) break;
		const float3 uvw = LocalToTex3DSpace(pos);

		// Get a sample
		min16float4 color = GetSample(uvw);

		// Skip empty space
		if (color.w > ZERO_THRESHOLD)
		{
#ifdef _POINT_LIGHT_
			// Point light direction in texture space
			const float3 lightStep = normalize(localSpaceLightPt - pos) * g_lightStepScale;
#endif
			// Sample light
			const float3 light = GetLight(pos, lightStep);

			// Accumulate color
			color.w = GetOpacity(color.w, g_stepScale);
			color.xyz *= transm * color.w;

			//scatter += color.xyz;
			scatter += min16float3(light) * color.xyz;

			// Attenuate ray-throughput
			transm *= 1.0 - color.w;
			if (transm < ZERO_THRESHOLD) break;
		}

		t += max(1.5 * g_stepScale * t, g_stepScale);
		if (t > tMax) break;
	}

	g_rwCubeMap[DTid] = float4(scatter, 1.0 - transm);
}
