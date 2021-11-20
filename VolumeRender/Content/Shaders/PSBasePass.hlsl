//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#ifdef _HAS_LIGHT_PROBE_
#define SH_ORDER 3
#include "SHIrradiance.hlsli"
#endif

#define IRRADIANCE_BIT	1
#define RADIANCE_BIT	2

//--------------------------------------------------------------------------------------
// Struct
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4	Pos		: SV_POSITION;
	float3	WSPos	: POSWORLD;
	float3	Norm	: NORMAL;
	float4	LSPos	: POSLIGHT;
	float4	CSPos	: POSCURRENT;
	float4	TSPos	: POSHISTORY;
};

struct PSOut
{
	min16float4 Color		: SV_TARGET0;
	min16float4 Velocity	: SV_TARGET1;
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

#ifdef _HAS_LIGHT_PROBE_
cbuffer cbSampleRes
{
	uint g_hasLightProbes;
};
#endif

static min16float3 baseColor = { 1.0, 0.6, 0.2 };

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
Texture2D<float> g_txDepth;
#ifdef _HAS_LIGHT_PROBE_
StructuredBuffer<float3> g_roSHCoeffs;
TextureCube<float3> g_txRadiance;
#endif

//--------------------------------------------------------------------------------------
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerComparisonState g_smpShadow;
#ifdef _HAS_LIGHT_PROBE_
SamplerState g_smpLinear;
#endif

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
PSOut main(PSIn input)
{
	PSOut output;

	const float shadow = ShadowMap(input.LSPos.xyz);

	const min16float3 N = min16float3(normalize(input.Norm));
#ifdef _HAS_LIGHT_PROBE_
	float3 irradiance = 0.0;
	const bool hasIrradiance = g_hasLightProbes & IRRADIANCE_BIT;
	if (hasIrradiance) irradiance = EvaluateSHIrradiance(g_roSHCoeffs, N);
#endif

	const float2 csPos = input.CSPos.xy / input.CSPos.w;
	const float2 tsPos = input.TSPos.xy / input.TSPos.w;
	const min16float2 velocity = min16float2(csPos - tsPos) * min16float2(0.5, -0.5);

	const min16float3 L = min16float3(normalize(g_lightPos));
	const min16float NoL = saturate(dot(N, L));

	const min16float3 V = min16float3(normalize(g_eyePos - input.WSPos));
	float3 radiance = 0.0;
#ifdef _HAS_LIGHT_PROBE_
	const min16float3 R = reflect(-V, N);
	const bool hasRadiance = g_hasLightProbes & RADIANCE_BIT;
	if (hasRadiance) radiance = g_txRadiance.SampleBias(g_smpLinear, R, 2.0);
#endif

	// Specular related
	const min16float3 H = normalize(V + L);
	const min16float NoH = saturate(dot(N, H));
	const min16float NoV = saturate(dot(N, V));

	const min16float3 lightColor = min16float3(g_lightColor.xyz * g_lightColor.w);
	min16float3 ambient = min16float3(g_ambient.xyz * g_ambient.w);
	ambient *= lerp(0.5, 1.0, N.y * 0.5 + 0.5);
#ifdef _HAS_LIGHT_PROBE_
	// Irradiance
	ambient = hasIrradiance ? min16float3(irradiance) : ambient;

	// Radiance
	const min16float roughness = 0.4;
	const min16float4 c0 = { -1.0, -0.0275, -0.572, 0.022 };
	const min16float4 c1 = { 1.0, 0.0425, 1.04, -0.04 };
	const min16float4 r = roughness * c0 + c1;
	const min16float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
	min16float2 amb = min16float2(-1.04, 1.04) * a004 + r.zw;
	radiance *= 0.04 * amb.x + amb.y;
#endif

	min16float3 result = baseColor * NoL;
	result += pow(NoH, 64.0) * Fresnel(NoV, 0.08) * PI;
	result *= lightColor * min16float(shadow);
	result += baseColor * ambient + min16float3(radiance);

	output.Color = min16float4(result, 1.0);
	output.Velocity = min16float4(velocity, 0.0.xx);

	return output;
}
