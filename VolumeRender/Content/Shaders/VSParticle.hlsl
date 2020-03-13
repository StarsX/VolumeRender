//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Structures
//--------------------------------------------------------------------------------------
struct Particle
{
	float4 Pos;
	float4 Color;
};

struct VSOut
{
	float4 Pos : SV_POSITION;
	float3 Tex : TEXCOORD;
	float3 Color : COLOR;
	float2 Domain : DOMAIN;
};

//--------------------------------------------------------------------------------------
// Constants
//--------------------------------------------------------------------------------------
cbuffer cbPerObject
{
	matrix g_worldView;
	matrix g_proj;
};

static const float g_particleRadius = 0.5;

//--------------------------------------------------------------------------------------
// Buffer
//--------------------------------------------------------------------------------------
StructuredBuffer<Particle> g_roParticles;

//--------------------------------------------------------------------------------------
// Vertex shader
//--------------------------------------------------------------------------------------
VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	VSOut output;

	const Particle particle = g_roParticles[iid];

	output.Pos = mul(particle.Pos, g_worldView);

	// Caculate position offset
	output.Domain = float2(vid & 1, vid >> 1);
	float2 offset = output.Domain * 2.0 - 1.0;
	offset.y = -offset.y;
	offset *= g_particleRadius;

	// Caculate vertex position
	output.Pos.xy += offset;

	// Output data
	output.Pos = mul(output.Pos, g_proj);
	output.Tex = particle.Pos.xyz * 0.5 + 0.5;
	output.Tex.y = 1.0 - output.Tex.y;
	output.Color = particle.Color.xyz;

	return output;
}
