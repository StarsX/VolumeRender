//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

float DepthWeight0(float z, float pSize)
{
	const float zInv = 1.0 - z / pSize;

	return max(zInv * zInv * zInv, 1e-7);
}

float DepthWeight1(float w)
{
	float z_sq_term = w / 5.0;
	float z_6_term = w / 200.0;

	z_sq_term *= z_sq_term;
	z_6_term *= z_6_term;
	z_6_term *= z_6_term * z_6_term;

	const float dnorm = z_6_term + z_sq_term + pow(10.0, -5.0);

	return clamp(10.0 / dnorm, 0.01, 3000.0);
}

float DepthWeight2(float w)
{
	float z_cb_term = w / 10.0;
	float z_6_term = w / 200.0;

	z_cb_term *= z_cb_term * z_cb_term;
	z_6_term *= z_6_term;
	z_6_term *= z_6_term * z_6_term;

	const float dnorm = z_6_term + z_cb_term + pow(10.0, -5.0);

	return clamp(10.0 / dnorm, 0.01, 3000.0);
}

float DepthWeight3(float w)
{
	float z_qd_term = w / 200.0;
	z_qd_term *= z_qd_term;
	z_qd_term *= z_qd_term;

	const float dnorm = z_qd_term + pow(10.0, -5.0);

	return clamp(0.03 / dnorm, 0.01, 3000.0);
}

float DepthWeight4(float z)
{
	const float zInv = 1.0 - z;

	return max(3000.0 * zInv * zInv * zInv, 0.01);
}
