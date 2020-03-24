//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define URAND_MAX 0xffff

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

	return (seed >> 0x10) & URAND_MAX;
}

//--------------------------------------------------------------------------------------
// Random number generator with a range
//--------------------------------------------------------------------------------------
uint rand(inout uint2 seed, uint range)
{
	return (rand(seed.x) | (rand(seed.y) << 16)) % range;
}

// Generate random unsigned int in [0, 2^24)
uint lcg(inout uint prev)
{
	const uint LCG_A = 1664525;
	const uint LCG_C = 1013904223;
	prev = (LCG_A * prev + LCG_C);

	return prev & 0x00ffffff;
}

// Generate random float in [0, 1)
float rnd(inout uint prev)
{
	return lcg(prev) / (float)0x01000000;
}

uint rot_seed(uint seed, uint frame)
{
	return seed ^ frame;
}
