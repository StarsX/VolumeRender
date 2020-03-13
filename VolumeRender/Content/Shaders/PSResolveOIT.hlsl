//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------
Texture2D g_txColor;
Texture2D<float> g_txTransm;

float4 main(PSIn input) : SV_TARGET
{
	const uint2 pos = input.Pos.xy;
	float4 color = g_txColor[pos];
	const float transm = g_txTransm[pos];

	color.xyz /= color.w;

	return float4(color.xyz, 1.0 - transm);
}
