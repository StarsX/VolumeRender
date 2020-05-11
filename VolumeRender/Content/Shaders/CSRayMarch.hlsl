//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "RayMarch.hlsli"

//--------------------------------------------------------------------------------------
// Constants
//--------------------------------------------------------------------------------------
static const min16float g_stepScale = g_maxDist / NUM_SAMPLES;
static const min16float g_lightStepScale = g_maxDist / NUM_LIGHT_SAMPLES;

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------
#ifdef _LIGHT_PASS_
Texture3D<float3> g_txLightMap;
#endif

//--------------------------------------------------------------------------------------
// Unordered access texture
//--------------------------------------------------------------------------------------
RWTexture2DArray<float4> g_rwHalvedCube;

//--------------------------------------------------------------------------------------
// Get the local-space position of the grid surface
//--------------------------------------------------------------------------------------
float3 GetLocalPos(float2 pos, uint slice, RWTexture2DArray<float4> rwHalvedCube)
{
	float3 gridSize;
	rwHalvedCube.GetDimensions(gridSize.x, gridSize.y, gridSize.z);
	
	pos = (pos + 0.5) / gridSize.xy * 2.0 - 1.0;
	pos.y = -pos.y;

	switch (slice)
	{
	case 0: // +X
		return float3(1.0, pos.yx);
	case 1: // -X
		return float3(-1.0, pos.y, -pos.x);
	case 2: // +Y
		return float3(pos.x, 1.0, pos.y);
	case 3: // -Y
		return float3(pos.x, -1.0, -pos.y);
	case 4: // +Z
		return float3(-pos.x, pos.y, 1.0);
	case 5: // -Z
		return float3(pos, -1.0);
	default:
		return 0.0;
	}
}

//--------------------------------------------------------------------------------------
// Get the local-space position of the grid surface
//--------------------------------------------------------------------------------------
float3 GetLocalPos(uint3 pos, float3 localSpaceEyePt, RWTexture2DArray<float4> rwHalvedCube)
{
	float4 focus = mul(float4(0.0.xxx, 1.0), g_worldViewProjI);
	focus.xyz /= focus.w;

	const float3 viewVec = localSpaceEyePt - focus.xyz;

	pos.z = viewVec[pos.z] < 0.0 ? pos.z * 2 + 1 : pos.z * 2;

	return GetLocalPos(pos.xy, pos.z, rwHalvedCube);
}

//--------------------------------------------------------------------------------------
// Compute Shader
//--------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	const float3 localSpaceEyePt = mul(g_eyePos, g_worldI).xyz;
	if (localSpaceEyePt[DTid.z] == 0.0) return;

	float3 pos = GetLocalPos(DTid, localSpaceEyePt, g_rwHalvedCube);
	float3 rayDir = normalize(pos - localSpaceEyePt);

	if (all(abs(localSpaceEyePt) <= 1.0))
	{
		pos = localSpaceEyePt;
		rayDir = -rayDir;
	}

	const float3 step = rayDir * g_stepScale;
#ifdef _POINT_LIGHT_
	const float3 localSpaceLightPt = mul(g_lightPos, g_worldI).xyz;
#else
	const float3 localSpaceLightPt = mul(g_lightPos.xyz, (float3x3)g_worldI);
	const float3 lightStep = normalize(localSpaceLightPt) * g_lightStepScale;
#endif

	// Transmittance
	min16float transm = 1.0;

	// In-scattered radiance
	min16float3 scatter = 0.0;

	for (uint i = 0; i < NUM_SAMPLES; ++i)
	{
		if (any(abs(pos) > 1.0)) break;
		float3 tex = pos * 0.5 + 0.5;

		// Get a sample
		min16float4 color = GetSample(tex);

		// Skip empty space
		if (color.w > ZERO_THRESHOLD)
		{
			// Point light direction in texture space
#ifdef _POINT_LIGHT_
			const float3 lightStep = normalize(localSpaceLightPt - pos) * g_lightStepScale;
#endif

			// Sample light
			float3 lightPos = pos + lightStep;
#ifdef _LIGHT_PASS_
			tex = lightPos * 0.5 + 0.5;
			const float3 light = g_txLightMap.SampleLevel(g_smpLinear, tex, 0.0);
#else
			min16float shadow = 1.0;	// Transmittance along light ray

			for (uint j = 0; j < NUM_LIGHT_SAMPLES; ++j)
			{
				if (any(abs(lightPos) > 1.0)) break;
				tex = lightPos * 0.5 + 0.5;

				// Get a sample along light ray
				const min16float density = GetSample(tex).w;

				// Attenuate ray-throughput along light direction
				shadow *= 1.0 - GetOpacity(density, g_lightStepScale);
				if (shadow < ZERO_THRESHOLD) break;

				// Update position along light ray
				lightPos += lightStep;
			}

			const min16float3 lightColor = min16float3(g_lightColor.xyz * g_lightColor.w);
			const min16float3 ambient = min16float3(g_ambient.xyz * g_ambient.w);
			const min16float3 light = lightColor * shadow + ambient;
#endif

			// Accumulate color
			color.w = GetOpacity(color.w, g_stepScale);
			color.xyz *= transm * color.w;
			//scatter += color.xyz;
			scatter += min16float3(light) * color.xyz;

			// Attenuate ray-throughput
			transm *= 1.0 - color.w;
			if (transm < ZERO_THRESHOLD) break;
		}

		pos += step;
	}

	g_rwHalvedCube[DTid] = float4(scatter, 1.0 - transm);
}
