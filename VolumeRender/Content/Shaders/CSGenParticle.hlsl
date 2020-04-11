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
	uint seed = rot_seed(DTid, 0);

	Particle particle;
	float3 tex;
	
	for (uint i = 0; i < 1024; ++i)
	{
		tex.x = rnd(seed);
		tex.y = rnd(seed);
		tex.z = rnd(seed);

		particle.Color = g_txGrid.SampleLevel(g_smpLinear, tex, 0.0);
		//if (rnd(seed) <= particle.Color.w) break;
		//if (particle.Color.w > 0.125) break;
		if (max(rnd(seed), 0.125) <= particle.Color.w) break;
	}

	particle.Pos = float4(tex * 2.0 - 1.0, 1.0);

	g_rwParticles[DTid] = particle;
}
