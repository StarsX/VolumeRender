
struct PSIn
{
	float4	Pos		: SV_POSITION;
	float3	WSPos	: POSWORLD;
	float3	Norm	: NORMAL;
};

min16float4 main(PSIn input) : SV_TARGET
{
	return min16float4(input.Norm, 1.0);
}
