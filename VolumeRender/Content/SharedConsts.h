//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

// _CPU_SLICE_CULL_: 0 - GPU culling; 1 - CPU computed visibility mask; 2 - CPU computed indexed slice list
#define _CPU_SLICE_CULL_ 1

static const float g_zNear = 1.0f;
static const float g_zFar = 1000.0f;
