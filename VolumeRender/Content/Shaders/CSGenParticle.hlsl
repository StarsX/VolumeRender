//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Random.hlsli"

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct Particle
{
	float4 Pos;
	float4 Color;
};

//--------------------------------------------------------------------------------------
// Texture and buffer
//--------------------------------------------------------------------------------------
Texture3D g_txGrid;
RWStructuredBuffer<Particle> g_rwParticles;

//--------------------------------------------------------------------------------------
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerState g_smpLinear;

[numthreads(64, 1, 1)]
void main(uint DTid : SV_DispatchThreadID)
{
	uint2 seed = { DTid, 0 };

	Particle particle;
	float3 tex;
	
	for (uint i = 0; i < 1024; ++i)
	{
		tex.x = rand(seed, 10000) / 9999.0;
		tex.y = rand(seed, 10000) / 9999.0;
		tex.z = rand(seed, 10000) / 9999.0;

		particle.Color = g_txGrid.SampleLevel(g_smpLinear, tex, 0.0);
		//if (rand(seed, 10000) / 9999.0 <= particle.Color.w) break;
		//if (particle.Color.w > 0.25) break;
		if (max(rand(seed, 10000) / 9999.0, 0.125) <= particle.Color.w) break;
	}

	particle.Pos = float4(tex * 2.0 - 1.0, 1.0);

	g_rwParticles[DTid] = particle;
}
