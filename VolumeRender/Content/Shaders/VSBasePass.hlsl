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
	float4	LSPos	: POSLIGHT;
	float4	CSPos	: POSCURRENT;
	float4	TSPos 	: POSHISTORY;
};

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cbPerObject
{
	float4x4 g_worldViewProj;
	float4x4 g_worldViewProjPrev;
	float4x3 g_world;
	float4x4 g_shadowWVP;
	float2 g_projBias;
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
	output.LSPos = mul(pos, g_shadowWVP);
	output.TSPos = mul(pos, g_worldViewProjPrev);
	output.CSPos = output.Pos;

	output.Pos.xy += g_projBias * output.Pos.w;
	output.Norm = mul(input.Nrm, (float3x3)g_world);

	return output;
}
