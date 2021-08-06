//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4 Pos	: SV_POSITION;
	float2 UV	: TEXCOORD;
};

Texture2D g_txSource;

min16float3 main(PSIn input) : SV_TARGET
{
	const float4 src = g_txSource[input.Pos.xy];
	min16float3 result = min16float3(src.xyz);
	result *= 1.25 / (result + 1.0);
	result = pow(abs(result), 1.25);

	return result;
}
