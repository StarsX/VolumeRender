//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Structs
//--------------------------------------------------------------------------------------
struct VSIn
{
	float3	Pos	: POSITION;
	float3	Nrm	: NORMAL;
};

struct VSOut
{
	float4	Pos		: SV_POSITION;
	float3	WSPos	: POSWORLD;
	float3	Norm	: NORMAL;
};

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cbPerObject
{
	matrix	g_worldViewProj;
	float4x3 g_world;
};

//--------------------------------------------------------------------------------------
// Base geometry pass
//--------------------------------------------------------------------------------------
VSOut main(VSIn input)
{
	VSOut output;

	const float4 pos = { input.Pos, 1.0 };
	output.Pos = mul(pos, g_worldViewProj);
	output.WSPos = mul(pos, g_world);
	output.Norm = mul(input.Nrm, (float3x3)g_world);

	return output;
}
