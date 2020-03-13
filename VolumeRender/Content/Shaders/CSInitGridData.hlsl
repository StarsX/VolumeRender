//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define RAND_MAX 0xffff

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------
RWTexture3D<float4> g_rwGrid;

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

	return (seed >> 0x10)& RAND_MAX;
}

//--------------------------------------------------------------------------------------
// Random number generator with a range
//--------------------------------------------------------------------------------------
uint rand(inout uint2 seed, uint range)
{
	return (rand(seed.x) | (rand(seed.y) << 16)) % range;
}

[numthreads(4, 4, 4)]
void main( uint3 DTid : SV_DispatchThreadID )
{
	float3 gridSize;
	g_rwGrid.GetDimensions(gridSize.x, gridSize.y, gridSize.z);

	const float3 pos = (DTid + 0.5) / gridSize * 2.0 - 1.0;
	const float r_sq = dot(pos, pos);
	float a = 1.0 - r_sq;
	a *= a;
	a = saturate(a * a * 2.0);

	g_rwGrid[DTid] = float4(0.5, 0.8, 1.0, a);
}
