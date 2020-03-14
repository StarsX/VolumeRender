//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define RAND_MAX 0xffff

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

//--------------------------------------------------------------------------------------
// Random number generator
//--------------------------------------------------------------------------------------
uint rand(inout uint seed)
{
	// The same implementation of current Windows rand()
	// msvcrt.dll: 77C271D8 mov     ecx, [eax + 14h]
	// msvcrt.dll: 77C271DB imul    ecx, 343FDh
	// msvcrt.dll: 77C271E1 add     ecx, 269EC3h
	// msvcrt.dll: 77C271E7 mov     [eax + 14h], ecx
	// msvcrt.dll: 77C271EA mov     eax, ecx
	// msvcrt.dll: 77C271EC shr     eax, 10h
	// msvcrt.dll: 77C271EF and     eax, 7FFFh
	seed = seed * 0x343fd + 0x269ec3;   // a = 214013, b = 2531011

	return (seed >> 0x10) & RAND_MAX;
}

//--------------------------------------------------------------------------------------
// Random number generator with a range
//--------------------------------------------------------------------------------------
uint rand(inout uint2 seed, uint range)
{
	return (rand(seed.x) | (rand(seed.y) << 16)) % range;
}

[numthreads(64, 1, 1)]
void main(uint DTid : SV_DispatchThreadID)
{
	uint2 seed = { DTid, 0 };

	Particle particle;
	float3 tex;
	
	for (uint i = 0; i < 64; ++i)
	{
		tex.x = rand(seed, 10000) / 9999.0;
		tex.y = rand(seed, 10000) / 9999.0;
		tex.z = rand(seed, 10000) / 9999.0;

		particle.Color = g_txGrid.SampleLevel(g_smpLinear, tex, 0.0);
		//if (rand(seed, 10000) / 9999.0 <= particle.Color.w) break;
		//if (particle.Color.w > 0.25) break;
		if (max(rand(seed, 10000) / 9999.0, 0.25) <= particle.Color.w) break;
	}

	particle.Pos = float4(tex * 2.0 - 1.0, 1.0);
	particle.Pos.y = particle.Pos.y;

	g_rwParticles[DTid] = particle;
}
