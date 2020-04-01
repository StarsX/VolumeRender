//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
Texture3D<float> g_txGrid;
RWTexture3D<float4> g_rwGrid;

//--------------------------------------------------------------------------------------
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerState g_smpLinear;

[numthreads(4, 4, 4)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	float3 gridSize;
	g_rwGrid.GetDimensions(gridSize.x, gridSize.y, gridSize.z);

	const float3 tex = (DTid + 0.5) / gridSize;
	const float a = g_txGrid.SampleLevel(g_smpLinear, tex, 0.0);

	g_rwGrid[DTid] = float4(1.0.xxx, a * 2.0);
}
