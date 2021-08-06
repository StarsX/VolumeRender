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
	float4	LSPos	: POSLIGHT;
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

static min16float3 baseColor = { 1.0, 0.6, 0.2 };

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
Texture2D<float> g_txDepth;

//--------------------------------------------------------------------------------------
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerComparisonState g_smpShadow;

//--------------------------------------------------------------------------------------
// Shadow mapping
//--------------------------------------------------------------------------------------
float ShadowMap(float3 pos)
{
	float2 uv = pos.xy * 0.5 + 0.5;
	uv.y = 1.0 - uv.y;

	return g_txDepth.SampleCmpLevelZero(g_smpShadow, uv, pos.z - 0.0027);
}

//--------------------------------------------------------------------------------------
// Schlick's approximation
//--------------------------------------------------------------------------------------
min16float3 Fresnel(min16float NoV, min16float3 specRef)
{
	const min16float fresnel = pow(1.0 - NoV, 5.0);

	return lerp(fresnel, 1.0, specRef);
}

//--------------------------------------------------------------------------------------
// Pixel shader
//--------------------------------------------------------------------------------------
min16float4 main(PSIn input) : SV_TARGET
{
	const float shadow = ShadowMap(input.LSPos.xyz);

	const min16float3 N = min16float3(normalize(input.Norm));
	const min16float3 L = min16float3(normalize(g_lightPos));
	const min16float NoL = saturate(dot(N, L));

	const min16float3 V = min16float3(normalize(g_eyePos - input.WSPos));
	const min16float3 H = normalize(V + L);
	const min16float NoH = saturate(dot(N, H));
	const min16float NoV = saturate(dot(N, V));

	const min16float3 lightColor = min16float3(g_lightColor.xyz * g_lightColor.w);
	const min16float3 ambient = min16float3(g_ambient.xyz * g_ambient.w);
	min16float3 result = baseColor * NoL;
	result += pow(NoH, 64.0) * Fresnel(NoV, 0.08) * PI;
	result *= lightColor * min16float(shadow);
	result += baseColor * ambient * lerp(0.5, 1.0, N.y * 0.5 + 0.5);

	return min16float4(result, 1.0);
}
