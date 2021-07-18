//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define PI 3.1415926535897

//--------------------------------------------------------------------------------------
// Struct
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4	Pos		: SV_POSITION;
	float3	WSPos	: POSWORLD;
	float3	Norm	: NORMAL;
};

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cbPerFrame
{
	float3 g_eyePos;
	float3 g_lightPos;
	float4 g_lightColor;
	float4 g_ambient;
};

static min16float3 baseColor = { 0.5, 1.0, 0.1 };

min16float4 main(PSIn input) : SV_TARGET
{
	const min16float3 N = min16float3(normalize(input.Norm));
	const min16float3 L = min16float3(normalize(g_lightPos));
	const min16float NoL = saturate(dot(N, L));

	const min16float3 lightColor = min16float3(g_lightColor.xyz * g_lightColor.w);
	const min16float3 ambient = min16float3(g_ambient.xyz * g_ambient.w);
	const min16float3 result = baseColor * (NoL * lightColor + ambient);

	return min16float4(result, 1.0);
}
