//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "RayCast.hlsli"

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct VSOut
{
	float4 Pos	: SV_POSITION;
	float3 LPt	: POSLOCAL;
	float3 UVW	: TEXCOORD;
};

static const float3x3 planes[6] =
{
	// back plane
	float3x3(-1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f),
	// left plane
	float3x3(0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f),
	// front plane
	float3x3(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f),
	// right plane
	float3x3(0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f),
	// top plane
	float3x3(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f),
	// bottom plane
	float3x3(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, -1.0f, 0.0f)
};

//--------------------------------------------------------------------------------------
// Vertex shader used for screen-space post-processing
//--------------------------------------------------------------------------------------
VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	VSOut output;

	const uint cubeID = iid / 6;
	const uint planeID = iid % 6;

	output.UVW = float3(vid & 1, vid >> 1, planeID);

	const float2 pos2D = output.UVW.xy * 2.0 - 1.0;
	float3 pos = float3(pos2D.x, -pos2D.y, 1.0);
	pos = mul(pos, planes[planeID]);

	// [TODO] can be fixed in planes[]
	if (planeID < 4) output.UVW.xy = 1.0 - output.UVW.xy;

	output.Pos = mul(float4(pos, 1.0), g_worldViewProj);
	output.LPt = pos;

	return output;
}
