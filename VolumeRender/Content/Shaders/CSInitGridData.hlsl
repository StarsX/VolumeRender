//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------
RWTexture3D<float4> g_rwGrid;

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

	const float3 colorU = float3(1.0, 0.6, 0.0);
	const float3 colorD = float3(0.5, 0.8, 1.0);
	const float3 color = lerp(colorD, colorU, saturate(pos.y * 0.5 + 0.2));

	g_rwGrid[DTid] = float4(color, a);
}
