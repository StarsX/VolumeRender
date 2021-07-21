//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "PSCube.hlsli"

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4 Pos	: SV_POSITION;
	float3 UVW	: TEXCOORD;
	float3 LPt	: POSLOCAL;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
min16float4 main(PSIn input) : SV_TARGET
{
	float2 gridSize;
	g_txCubeMap.GetDimensions(gridSize.x, gridSize.y);
	float3 uvw = input.UVW;
	float2 uv = uvw.xy;

#if !_USE_PURE_ARRAY_
	uvw = input.LPt;
#endif

	return CubeCast(uvw, uv * gridSize, input.Pos.xy);
}
