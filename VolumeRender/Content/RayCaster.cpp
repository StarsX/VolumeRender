//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen & ZENG, Wei. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConsts.h"
#include "RayCaster.h"
#define _INDEPENDENT_DDS_LOADER_
#include "Advanced/XUSGDDSLoader.h"
#undef _INDEPENDENT_DDS_LOADER_

using namespace std;
using namespace DirectX;
using namespace XUSG;

struct CBPerFrame
{
	XMFLOAT4 EyePos;
	XMFLOAT3X4 LightMapWorld;
	XMFLOAT4X4 ShadowViewProj;
	XMFLOAT4 LightPos;
	XMFLOAT4 LightColor;
	XMFLOAT4 Ambient;
};

struct CBPerObject
{
	XMFLOAT4X4 WorldViewProjI;
	XMFLOAT4X4 WorldViewProj;
	XMFLOAT3X4 WorldI;
	XMFLOAT3X4 World;
	XMFLOAT3X4 LocalToLight;
};

#ifdef _CPU_CUBE_FACE_CULL_
static_assert(_CPU_CUBE_FACE_CULL_ == 0 || _CPU_CUBE_FACE_CULL_ == 1 || _CPU_CUBE_FACE_CULL_ == 2, "_CPU_CUBE_FACE_CULL_ can only be 0, 1, or 2");
#endif

#if _CPU_CUBE_FACE_CULL_
static inline bool IsCubeFaceVisible(uint8_t face, CXMVECTOR localSpaceEyePt)
{
	const auto& viewComp = XMVectorGetByIndex(localSpaceEyePt, face >> 1);

	return (face & 0x1) ? viewComp > -1.0f : viewComp < 1.0f;
}
#endif

#if _CPU_CUBE_FACE_CULL_ == 1
static inline uint32_t GenVisibilityMask(CXMMATRIX worldI, const XMFLOAT3& eyePt)
{
	const auto localSpaceEyePt = XMVector3Transform(XMLoadFloat3(&eyePt), worldI);

	auto mask = 0u;
	for (uint8_t i = 0; i < 6; ++i)
	{
		const auto isVisible = IsCubeFaceVisible(i, localSpaceEyePt);
		mask |= (isVisible ? 1 : 0) << i;
	}

	return mask;
}
#elif _CPU_CUBE_FACE_CULL_ == 2
struct CBCubeFaceList
{
	XMUINT4 Faces[5];
};

static inline uint8_t GenVisibleCubeFaceList(CBCubeFaceList& faceList, CXMMATRIX worldI, const XMFLOAT3& eyePt)
{
	const auto localSpaceEyePt = XMVector3Transform(XMLoadFloat3(&eyePt), worldI);

	uint8_t count = 0;
	for (uint8_t i = 0; i < 6; ++i)
	{
		if (IsCubeFaceVisible(i, localSpaceEyePt))
		{
			assert(count < 5);
			faceList.Faces[count++].x = i;
		}
	}

	return count;
}
#endif

static inline XMVECTOR ProjectToViewport(uint32_t i, CXMMATRIX worldViewProj, CXMVECTOR viewport)
{
	static const XMVECTOR v[] =
	{
		XMVectorSet(1.0f, 1.0f, 1.0f, 1.0f),
		XMVectorSet(-1.0f, 1.0f, 1.0f, 1.0f),
		XMVectorSet(1.0f, -1.0f, 1.0f, 1.0f),
		XMVectorSet(-1.0f, -1.0f, 1.0f, 1.0f),
		
		XMVectorSet(-1.0f, 1.0f, -1.0f, 1.0f),
		XMVectorSet(1.0f, 1.0f, -1.0f, 1.0f),
		XMVectorSet(-1.0f, -1.0f, -1.0f, 1.0f),
		XMVectorSet(1.0f, -1.0f, -1.0f, 1.0f),
	};

	auto p = XMVector3TransformCoord(v[i], worldViewProj);
	p *= XMVectorSet(0.5f, -0.5f, 1.0f, 1.0f);
	p += XMVectorSet(0.5f, 0.5f, 0.0f, 0.0f);

	return p * viewport;
}

//static inline float EstimateCubeFacePixelSize(uint8_t vi[4], const XMVECTOR v[8])
//{
//	static const uint8_t order[] = { 0, 1, 3, 2 };
//
//	auto s = 0.0f;
//	for (uint8_t i = 0; i < 4; ++i)
//	{
//		const auto& j = vi[order[i]];
//		const auto e = v[(j + 1) % 4] - v[j];
//		s = (max)(XMVectorGetX(XMVector2Length(e)), s);
//	}
//
//	return s;
//}

static inline float EstimateCubeEdgePixelSize(const XMVECTOR v[8])
{
	static const uint8_t ei[][2] =
	{
		{ 0, 1 },
		{ 3, 2 },

		{ 1, 3 },
		{ 2, 0 },

		{ 4, 5 },
		{ 7, 6 },

		{ 5, 7 },
		{ 6, 4 },

		{ 1, 4 },
		{ 6, 3 },

		{ 5, 0 },
		{ 2, 7 }
	};

	auto s = 0.0f;
	for (uint8_t i = 0; i < 12; ++i)
	{
		const auto e = v[ei[i][1]] - v[ei[i][0]];
		s = (max)(XMVectorGetX(XMVector2Length(e)), s);
	}

	return s;
}

static inline uint8_t EstimateCubeMapLOD(uint32_t& raySampleCount, uint8_t numMips, float cubeMapSize,
	CXMMATRIX worldViewProj, CXMVECTOR viewport, float upscale = 2.0f, float raySampleCountScale = 2.0f)
{
	XMVECTOR v[8];
	for (uint8_t i = 0; i < 8; ++i) v[i] = ProjectToViewport(i, worldViewProj, viewport);

	// Calulate the ideal cube-map resolution
	auto s = EstimateCubeEdgePixelSize(v) / upscale;
	
	// Get the ideal ray sample amount
	auto raySampleAmt = raySampleCountScale * s / sqrtf(3.0f);

	// Clamp the ideal ray sample amount using the user-specified upper bound of ray sample count
	const auto raySampleCnt = static_cast<uint32_t>(ceilf(raySampleAmt));
	raySampleCount = (min)(raySampleCnt, raySampleCount);

	// Inversely derive the cube-map resolution from the clamped ray sample amount
	raySampleAmt = (min)(raySampleAmt, static_cast<float>(raySampleCount));
	s = raySampleAmt / raySampleCountScale * sqrtf(3.0f);

	// Use the more detailed integer level for conservation
	//const auto level = static_cast<uint8_t>(floorf((max)(log2f(cubeMapSize / s), 0.0f)));
	const auto level = static_cast<uint8_t>((max)(log2f(cubeMapSize / s), 0.0f));

	return min<uint8_t>(level, numMips - 1);
}

RayCaster::RayCaster() :
	m_pDepths(nullptr),
	m_coeffSH(nullptr),
	m_maxRaySamples(256),
	m_maxLightSamples(64),
	m_cubeFaceCount(6),
	m_cubeMapLOD(0),
	m_lightPt(75.0f, 75.0f, -75.0f),
	m_lightColor(1.0f, 0.7f, 0.3f, 1.0f),
	m_ambient(0.0f, 0.3f, 1.0f, 0.4f)
{
	m_shaderPool = ShaderPool::MakeUnique();

	XMStoreFloat3x4(&m_volumeWorld, XMMatrixScaling(10.0f, 10.0f, 10.0f));
	m_lightMapWorld = m_volumeWorld;
}

RayCaster::~RayCaster()
{
}

bool RayCaster::Init(const Device* pDevice, const DescriptorTableCache::sptr& descriptorTableCache,
	Format rtFormat, uint32_t gridSize, uint32_t lightGridSize, const DepthStencil::uptr* depths)
{
	m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(pDevice);
	m_computePipelineCache = Compute::PipelineCache::MakeUnique(pDevice);
	m_pipelineLayoutCache = PipelineLayoutCache::MakeUnique(pDevice);
	m_descriptorTableCache = descriptorTableCache;

	m_gridSize = gridSize;
	m_lightGridSize = lightGridSize;
	m_pDepths = depths;

	// Create resources
	m_volume = Texture3D::MakeUnique();
	XUSG_N_RETURN(m_volume->Create(pDevice, gridSize, gridSize, gridSize, Format::R16G16B16A16_FLOAT,
		ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::ALLOW_SIMULTANEOUS_ACCESS, 1,
		MemoryFlag::NONE, L"Volume"), false);

	const uint8_t numMips = 5;
	m_cubeMap = Texture2D::MakeUnique();
	XUSG_N_RETURN(m_cubeMap->Create(pDevice, gridSize, gridSize, Format::R16G16B16A16_FLOAT, 6,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, numMips, 1, true, MemoryFlag::NONE, L"RadianceCubeMap"), false);

	m_cubeDepth = Texture2D::MakeUnique();
	XUSG_N_RETURN(m_cubeDepth->Create(pDevice, gridSize, gridSize, Format::R32_FLOAT, 6,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, numMips, 1, true, MemoryFlag::NONE, L"DepthCubeMap"), false);

	m_lightMap = Texture3D::MakeUnique();
	XUSG_N_RETURN(m_lightMap->Create(pDevice, m_lightGridSize, m_lightGridSize, m_lightGridSize,
		Format::R11G11B10_FLOAT,ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::ALLOW_SIMULTANEOUS_ACCESS,
		1, MemoryFlag::NONE, L"LightMap"), false);

	m_cbPerFrame = ConstantBuffer::MakeUnique();
	XUSG_N_RETURN(m_cbPerFrame->Create(pDevice, sizeof(CBPerFrame[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"RayCaster.CBPerFrame"), false);

	m_cbPerObject = ConstantBuffer::MakeUnique();
	XUSG_N_RETURN(m_cbPerObject->Create(pDevice, sizeof(CBPerObject[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"RayCaster.CBPerObject"), false);

#if _CPU_CUBE_FACE_CULL_ == 2
	m_cbCubeFaceList = ConstantBuffer::MakeUnique();
	XUSG_N_RETURN(m_cbCubeFaceList->Create(m_device.get(), sizeof(CBCubeFaceList[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"RayCaster.CBCubeFaceList"), false);
#endif

	// Create pipelines
	XUSG_N_RETURN(createPipelineLayouts(), false);
	XUSG_N_RETURN(createPipelines(rtFormat), false);
	XUSG_N_RETURN(createDescriptorTables(), false);

	return true;
}

bool RayCaster::LoadVolumeData(CommandList* pCommandList, const wchar_t* fileName, vector<Resource::uptr>& uploaders)
{
	// Load input image
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		uploaders.emplace_back(Resource::MakeUnique());
		XUSG_N_RETURN(textureLoader.CreateTextureFromFile(pCommandList, fileName,
			8192, false, m_fileSrc, uploaders.back().get(), &alphaMode), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_fileSrc->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_FILE_SRC], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	ResourceBarrier barrier;
	m_volume->SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);

	const auto descriptorPool = m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL);
	pCommandList->SetDescriptorPools(1, &descriptorPool);

	// Set pipeline state
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[LOAD_VOLUME_DATA]);
	pCommandList->SetPipelineState(m_pipelines[LOAD_VOLUME_DATA]);

	// Set descriptor tables
	pCommandList->SetComputeDescriptorTable(0, m_srvTables[SRV_TABLE_FILE_SRC]);
	pCommandList->SetComputeDescriptorTable(1, m_uavTable);

	// Dispatch grid
	pCommandList->Dispatch(XUSG_DIV_UP(m_gridSize, 4), XUSG_DIV_UP(m_gridSize, 4), XUSG_DIV_UP(m_gridSize, 4));

	return true;
}

bool RayCaster::SetDepthMaps(const DepthStencil::uptr* depths)
{
	m_pDepths = depths;

	return createDescriptorTables();
}

void RayCaster::InitVolumeData(const CommandList* pCommandList)
{
	ResourceBarrier barrier;
	m_volume->SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);

	const DescriptorPool pDescriptorPool[] =
	{ m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL) };
	pCommandList->SetDescriptorPools(1, pDescriptorPool);

	// Set pipeline state
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[INIT_VOLUME_DATA]);
	pCommandList->SetPipelineState(m_pipelines[INIT_VOLUME_DATA]);

	// Set descriptor tables
	pCommandList->SetComputeDescriptorTable(0, m_uavTable);

	// Dispatch grid
	pCommandList->Dispatch(XUSG_DIV_UP(m_gridSize, 4), XUSG_DIV_UP(m_gridSize, 4), XUSG_DIV_UP(m_gridSize, 4));
}

void RayCaster::SetSH(const StructuredBuffer::sptr& coeffSH)
{
	m_coeffSH = coeffSH;
}

void RayCaster::SetMaxSamples(uint32_t maxRaySamples, uint32_t maxLightSamples)
{
	m_maxRaySamples = maxRaySamples;
	m_maxLightSamples = maxLightSamples;
}

void RayCaster::SetVolumeWorld(float size, const XMFLOAT3& pos, const XMFLOAT3* pPitchYawRoll)
{
	size *= 0.5f;
	auto world = XMMatrixScaling(size, size, size);
	if (pPitchYawRoll) world *= XMMatrixRotationRollPitchYaw(pPitchYawRoll->x, pPitchYawRoll->y, pPitchYawRoll->z);
	world = world * XMMatrixTranslation(pos.x, pos.y, pos.z);
	XMStoreFloat3x4(&m_volumeWorld, world);
}

void RayCaster::SetLightMapWorld(float size, const XMFLOAT3& pos)
{
	size *= 0.5f;
	auto world = XMMatrixScaling(size, size, size);
	world = world * XMMatrixTranslation(pos.x, pos.y, pos.z);
	XMStoreFloat3x4(&m_lightMapWorld, world);
}

void RayCaster::SetLight(const XMFLOAT3& pos, const XMFLOAT3& color, float intensity)
{
	m_lightPt = pos;
	m_lightColor = XMFLOAT4(color.x, color.y, color.z, intensity);
}

void RayCaster::SetAmbient(const XMFLOAT3& color, float intensity)
{
	m_ambient = XMFLOAT4(color.x, color.y, color.z, intensity);
}

void RayCaster::UpdateFrame(uint8_t frameIndex, CXMMATRIX viewProj, const XMFLOAT4X4& shadowVP, const XMFLOAT3& eyePt)
{
	// Per-frame
	{
		const auto pCbData = reinterpret_cast<CBPerFrame*>(m_cbPerFrame->Map(frameIndex));
		pCbData->EyePos = XMFLOAT4(eyePt.x, eyePt.y, eyePt.z, 1.0f);
		pCbData->LightMapWorld = m_lightMapWorld;
		pCbData->ShadowViewProj = shadowVP;
		pCbData->LightPos = XMFLOAT4(m_lightPt.x, m_lightPt.y, m_lightPt.z, 1.0f);
		pCbData->LightColor = m_lightColor;
		pCbData->Ambient = m_ambient;
	}

	// Per-object
	{
		const auto lightWorld = XMLoadFloat3x4(&m_lightMapWorld);
		const auto lightWorldI = XMMatrixInverse(nullptr, lightWorld);

		const auto world = XMLoadFloat3x4(&m_volumeWorld);
		const auto worldI = XMMatrixInverse(nullptr, world);
		const auto worldViewProj = world * viewProj;

		const auto pCbData = reinterpret_cast<CBPerObject*>(m_cbPerObject->Map(frameIndex));
		XMStoreFloat4x4(&pCbData->WorldViewProj, XMMatrixTranspose(worldViewProj));
		XMStoreFloat4x4(&pCbData->WorldViewProjI, XMMatrixTranspose(XMMatrixInverse(nullptr, worldViewProj)));
		XMStoreFloat3x4(&pCbData->WorldI, worldI);
		XMStoreFloat3x4(&pCbData->World, world);
		XMStoreFloat3x4(&pCbData->LocalToLight, world * lightWorldI);

		{
			m_raySampleCount = m_maxRaySamples;
			const auto& depth = m_pDepths[DEPTH_MAP];
			const auto numMips = m_cubeMap->GetNumMips();
			const auto cubeMapSize = static_cast<float>(m_cubeMap->GetWidth());
			const auto width = static_cast<float>(depth->GetWidth());
			const auto height = static_cast<float>(depth->GetHeight());
			const auto viewport = XMVectorSet(width, height, 1.0f, 1.0f);
			m_cubeMapLOD = EstimateCubeMapLOD(m_raySampleCount, numMips, cubeMapSize, worldViewProj, viewport);

#if _CPU_CUBE_FACE_CULL_ == 1
			m_visibilityMask = GenVisibilityMask(worldI, eyePt);
#elif _CPU_CUBE_FACE_CULL_ == 2
			{
				const auto pCbData = reinterpret_cast<CBCubeFaceList*>(m_cbCubeFaceList->Map(frameIndex));
				m_cubeFaceCount = GenVisibleCubeFaceList(*pCbData, worldI, eyePt);
			}
#endif
		}
	}
}

void RayCaster::Render(CommandList* pCommandList, uint8_t frameIndex, uint8_t flags)
{
	const bool cubemapRayMarch = flags & RAY_MARCH_CUBEMAP;
	const bool separateLightPass = flags & SEPARATE_LIGHT_PASS;

	if (cubemapRayMarch)
	{
		if (separateLightPass)
		{
			RayMarchL(pCommandList, frameIndex);
			rayMarchV(pCommandList, frameIndex);
		}
		else rayMarch(pCommandList, frameIndex);

		renderCube(pCommandList, frameIndex);
		//rayCastCube(pCommandList, frameIndex);
	}
	else
	{
		if (separateLightPass)
		{
			RayMarchL(pCommandList, frameIndex);
			rayCastVDirect(pCommandList, frameIndex);
		}
		else rayCastDirect(pCommandList, frameIndex);
	}
}

void RayCaster::RayMarchL(const CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barrier
	ResourceBarrier barrier;
	m_lightMap->SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);

	// Set pipeline state
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[RAY_MARCH_L]);
	pCommandList->SetPipelineState(m_pipelines[RAY_MARCH_L]);

	// Set descriptor tables
	pCommandList->SetComputeDescriptorTable(0, m_cbvTables[frameIndex]);
	pCommandList->SetComputeDescriptorTable(1, m_srvUavTable);
	pCommandList->SetComputeDescriptorTable(2, m_srvTables[SRV_TABLE_SHADOW]);
	pCommandList->SetCompute32BitConstant(3, m_maxLightSamples);
	pCommandList->SetCompute32BitConstant(3, m_coeffSH ? 1 : 0, 1);
	if (m_coeffSH) pCommandList->SetComputeRootShaderResourceView(4, m_coeffSH.get());

	// Dispatch grid
	pCommandList->Dispatch(XUSG_DIV_UP(m_lightGridSize, 4), XUSG_DIV_UP(m_lightGridSize, 4), XUSG_DIV_UP(m_lightGridSize, 4));
}

const DescriptorTable& RayCaster::GetVolumeSRVTable(CommandList* pCommandList)
{
	// Set barrier
	ResourceBarrier barrier;
	const auto numBarriers = m_volume->SetBarrier(&barrier, ResourceState::NON_PIXEL_SHADER_RESOURCE |
		ResourceState::PIXEL_SHADER_RESOURCE);
	pCommandList->Barrier(numBarriers, &barrier);

	return m_srvUavTable;
}

const DescriptorTable& RayCaster::GetLightSRVTable() const
{
	return m_srvTables[SRV_TABLE_LIGHT_MAP];
}

Resource* RayCaster::GetLightMap() const
{
	return m_lightMap.get();
}

bool RayCaster::createPipelineLayouts()
{
	const Sampler samplers[] =
	{
		m_descriptorTableCache->GetSampler(SamplerPreset::LINEAR_CLAMP),
		m_descriptorTableCache->GetSampler(SamplerPreset::POINT_CLAMP)
	};

	// Load grid data
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetStaticSamplers(samplers, 1, 0);
		XUSG_X_RETURN(m_pipelineLayouts[LOAD_VOLUME_DATA], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"LoadGridDataLayout"), false);
	}

	// Init grid data
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		XUSG_X_RETURN(m_pipelineLayouts[INIT_VOLUME_DATA], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"InitGridDataLayout"), false);
	}

	// Ray marching
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 2, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::UAV, 2, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetRange(3, DescriptorType::SRV, 2, 1);
		pipelineLayout->SetConstants(4, 3, 2);
		pipelineLayout->SetRootSRV(5, 3);
#if _CPU_CUBE_FACE_CULL_ == 1
		pipelineLayout->SetConstants(6, 1, 3);
#elif _CPU_CUBE_FACE_CULL_ == 2
		pipelineLayout->SetRootCBV(6, 3);
#endif
		pipelineLayout->SetStaticSamplers(samplers, 1, 0);
		XUSG_X_RETURN(m_pipelineLayouts[RAY_MARCH], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"RayMarchingLayout"), false);
	}

	// Light space ray marching
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 2, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 1, 1);
		pipelineLayout->SetConstants(3, 2, 2);
		pipelineLayout->SetRootSRV(4, 2);
		pipelineLayout->SetStaticSamplers(samplers, 1, 0);
		XUSG_X_RETURN(m_pipelineLayouts[RAY_MARCH_L], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"LightSpaceRayMarchingLayout"), false);
	}

	// View space ray marching
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 2, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::UAV, 2, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 2, 0);
		pipelineLayout->SetRange(3, DescriptorType::SRV, 1, 2);
		pipelineLayout->SetConstants(4, 1, 2);
#if _CPU_CUBE_FACE_CULL_ == 1
		pipelineLayout->SetConstants(5, 1, 3);
#elif _CPU_CUBE_FACE_CULL_ == 2
		pipelineLayout->SetRootCBV(5, 3);
#endif
		pipelineLayout->SetStaticSamplers(samplers, 2, 0);
		XUSG_X_RETURN(m_pipelineLayouts[RAY_MARCH_V], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"ViewSpaceRayMarchingLayout"), false);
	}

	// Cube rendering
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 2, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 2, 0);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 1, 2);
		pipelineLayout->SetStaticSamplers(samplers, 1, 0, 0, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(1, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(2, Shader::Stage::PS);
		XUSG_X_RETURN(m_pipelineLayouts[RENDER_CUBE], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"CubeRenderingLayout"), false);
	}

	// Screen-space ray casting from cube map
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 2, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 2, 0);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 1, 2);
		pipelineLayout->SetStaticSamplers(samplers, 1, 0, 0, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(1, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(2, Shader::Stage::PS);
		XUSG_X_RETURN(m_pipelineLayouts[RAY_CAST], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"RayCastingLayout"), false);
	}

	// Direct ray casting
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 2, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 2, 1);
		pipelineLayout->SetConstants(3, 3, 2, 0, Shader::Stage::PS);
		pipelineLayout->SetRootSRV(4, 3, 0, DescriptorFlag::NONE, Shader::Stage::PS);
		pipelineLayout->SetStaticSamplers(samplers, 1, 0, 0, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(1, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(2, Shader::Stage::PS);
		XUSG_X_RETURN(m_pipelineLayouts[DIRECT_RAY_CAST], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"DirectRayCastingLayout"), false);
	}

	// View space direct ray casting
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 2, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 2, 0);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 1, 2);
		pipelineLayout->SetConstants(3, 1, 2, 0, Shader::Stage::PS);
		pipelineLayout->SetStaticSamplers(samplers, 1, 0, 0, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(1, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(2, Shader::Stage::PS);
		XUSG_X_RETURN(m_pipelineLayouts[DIRECT_RAY_CAST_V], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"ViewSpaceDirectRayCastingLayout"), false);
	}

	return true;
}

bool RayCaster::createPipelines(Format rtFormat)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	// Load grid data
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSR32FToRGBA16F.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[LOAD_VOLUME_DATA]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[LOAD_VOLUME_DATA], state->GetPipeline(m_computePipelineCache.get(), L"InitGridData"), false);
	}

	// Init grid data
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSInitGridData.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[INIT_VOLUME_DATA]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[INIT_VOLUME_DATA], state->GetPipeline(m_computePipelineCache.get(), L"InitGridData"), false);
	}

	// Ray marching
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSRayMarch.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_MARCH]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[RAY_MARCH], state->GetPipeline(m_computePipelineCache.get(), L"RayMarching"), false);
	}

	// Light space ray marching
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSRayMarchL.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_MARCH_L]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[RAY_MARCH_L], state->GetPipeline(m_computePipelineCache.get(), L"LightSpaceRayMarching"), false);
	}

	// View space ray marching
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSRayMarchV.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_MARCH_V]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[RAY_MARCH_V], state->GetPipeline(m_computePipelineCache.get(), L"ViewSpaceRayMarching"), false);
	}

	// Cube rendering
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSCube.cso"), false);
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSCube.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RENDER_CUBE]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->RSSetState(Graphics::CULL_FRONT, m_graphicsPipelineCache.get()); // Front-face culling for interior surfaces
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->OMSetBlendState(Graphics::PREMULTIPLITED, m_graphicsPipelineCache.get());
		state->OMSetRTVFormats(&rtFormat, 1);
		XUSG_X_RETURN(m_pipelines[RENDER_CUBE], state->GetPipeline(m_graphicsPipelineCache.get(), L"CubeRendering"), false);
	}

	XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);

	// Screen-space ray casting from cube map
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSRayCastCube.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_CAST]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->OMSetBlendState(Graphics::PREMULTIPLITED, m_graphicsPipelineCache.get());
		state->OMSetRTVFormats(&rtFormat, 1);
		XUSG_X_RETURN(m_pipelines[RAY_CAST], state->GetPipeline(m_graphicsPipelineCache.get(), L"RayCasting"), false);
	}

	// Direct ray casting
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSRayCast.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[DIRECT_RAY_CAST]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->OMSetBlendState(Graphics::PREMULTIPLITED, m_graphicsPipelineCache.get());
		state->OMSetRTVFormats(&rtFormat, 1);
		XUSG_X_RETURN(m_pipelines[DIRECT_RAY_CAST], state->GetPipeline(m_graphicsPipelineCache.get(), L"DirectRayCasting"), false);
	}

	// View space direct ray casting
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSRayCastV.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[DIRECT_RAY_CAST_V]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->OMSetBlendState(Graphics::PREMULTIPLITED, m_graphicsPipelineCache.get());
		state->OMSetRTVFormats(&rtFormat, 1);
		XUSG_X_RETURN(m_pipelines[DIRECT_RAY_CAST_V], state->GetPipeline(m_graphicsPipelineCache.get(), L"ViewSpaceDirectRayCasting"), false);
	}

	return true;
}

bool RayCaster::createDescriptorTables()
{
	// Create CBV tables
	for (uint8_t i = 0; i < FrameCount; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const Descriptor descriptors[] =
		{
			m_cbPerObject->GetCBV(i),
			m_cbPerFrame->GetCBV(i)
		};
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		XUSG_X_RETURN(m_cbvTables[i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create UAV and SRV table
	const uint8_t numMips = m_cubeMap->GetNumMips();
	m_uavMipTables.resize(numMips);
	for (uint8_t i = 0; i < numMips; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const Descriptor descriptors[] =
		{
			m_cubeMap->GetUAV(i),
			m_cubeDepth->GetUAV(i)
		};
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		XUSG_X_RETURN(m_uavMipTables[i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create SRV tables
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_volume->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_VOLUME], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_lightMap->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_LIGHT_MAP], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create SRV and UAV table
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const Descriptor descriptors[] =
		{
			m_volume->GetSRV(),
			m_lightMap->GetUAV()
		};
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		XUSG_X_RETURN(m_srvUavTable, descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create SRV tables
	m_srvMipTables.resize(numMips);
	for (uint8_t i = 0; i < numMips; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const Descriptor descriptors[] =
		{
			m_cubeMap->GetSRV(i),
			m_cubeDepth->GetSRV(i)
		};
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		XUSG_X_RETURN(m_srvMipTables[i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	if (m_pDepths)
	{
		if (m_pDepths[DEPTH_MAP])
		{
			const auto descriptorTable = Util::DescriptorTable::MakeUnique();
			descriptorTable->SetDescriptors(0, 1, &m_pDepths[DEPTH_MAP]->GetSRV());
			XUSG_X_RETURN(m_srvTables[SRV_TABLE_DEPTH], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
		}

		if (m_pDepths[SHADOW_MAP])
		{
			const auto descriptorTable = Util::DescriptorTable::MakeUnique();
			descriptorTable->SetDescriptors(0, 1, &m_pDepths[SHADOW_MAP]->GetSRV());
			XUSG_X_RETURN(m_srvTables[SRV_TABLE_SHADOW], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
		}
	}

	// Create UAV table
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_volume->GetUAV());
		XUSG_X_RETURN(m_uavTable, descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	return true;
}

void RayCaster::rayMarch(CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	ResourceBarrier barriers[13];
	auto numBarriers = m_pDepths[DEPTH_MAP]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE |
		ResourceState::PIXEL_SHADER_RESOURCE);
	for (uint8_t i = 0; i < 6; ++i)
	{
		numBarriers = m_cubeMap->SetBarrier(barriers, m_cubeMapLOD, ResourceState::UNORDERED_ACCESS, numBarriers, i);
		numBarriers = m_cubeDepth->SetBarrier(barriers, m_cubeMapLOD, ResourceState::UNORDERED_ACCESS, numBarriers, i);
	}
	pCommandList->Barrier(numBarriers, barriers);

	// Set pipeline state
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[RAY_MARCH]);
	pCommandList->SetPipelineState(m_pipelines[RAY_MARCH]);

	// Set descriptor tables
	pCommandList->SetComputeDescriptorTable(0, m_cbvTables[frameIndex]);
	pCommandList->SetComputeDescriptorTable(1, m_uavMipTables[m_cubeMapLOD]);
	pCommandList->SetComputeDescriptorTable(2, m_srvTables[SRV_TABLE_VOLUME]);
	pCommandList->SetComputeDescriptorTable(3, m_srvTables[SRV_TABLE_DEPTH]);
	pCommandList->SetCompute32BitConstant(4, m_raySampleCount);
	pCommandList->SetCompute32BitConstant(4, m_coeffSH ? 1 : 0, 1);
	pCommandList->SetCompute32BitConstant(4, m_maxLightSamples, 2);
	if (m_coeffSH) pCommandList->SetComputeRootShaderResourceView(5, m_coeffSH.get());
#if _CPU_CUBE_FACE_CULL_ == 1
	pCommandList->SetCompute32BitConstant(6, m_visibilityMask);
#elif _CPU_CUBE_FACE_CULL_ == 2
	pCommandList->SetComputeRootConstantBufferView(5, m_cbCubeFaceList.get(), m_cbCubeFaceList->GetCBVOffset(frameIndex));
#endif

	// Dispatch cube
	const auto gridSize = m_gridSize >> m_cubeMapLOD;
	pCommandList->Dispatch(XUSG_DIV_UP(gridSize, 8), XUSG_DIV_UP(gridSize, 8), m_cubeFaceCount);
}

void RayCaster::rayMarchV(CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	ResourceBarrier barriers[14];
	auto numBarriers = m_lightMap->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE);
	numBarriers = m_pDepths[DEPTH_MAP]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE |
		ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	for (uint8_t i = 0; i < 6; ++i)
	{
		numBarriers = m_cubeMap->SetBarrier(barriers, m_cubeMapLOD, ResourceState::UNORDERED_ACCESS, numBarriers, i);
		numBarriers = m_cubeDepth->SetBarrier(barriers, m_cubeMapLOD, ResourceState::UNORDERED_ACCESS, numBarriers, i);
	}
	pCommandList->Barrier(numBarriers, barriers);

	// Set pipeline state
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[RAY_MARCH_V]);
	pCommandList->SetPipelineState(m_pipelines[RAY_MARCH_V]);

	// Set descriptor tables
	pCommandList->SetComputeDescriptorTable(0, m_cbvTables[frameIndex]);
	pCommandList->SetComputeDescriptorTable(1, m_uavMipTables[m_cubeMapLOD]);
	pCommandList->SetComputeDescriptorTable(2, m_srvTables[SRV_TABLE_VOLUME]);
	pCommandList->SetComputeDescriptorTable(3, m_srvTables[SRV_TABLE_DEPTH]);
	pCommandList->SetCompute32BitConstant(4, m_raySampleCount);
#if _CPU_CUBE_FACE_CULL_ == 1
	pCommandList->SetCompute32BitConstant(5, m_visibilityMask);
#elif _CPU_CUBE_FACE_CULL_ == 2
	pCommandList->SetComputeRootConstantBufferView(5, m_cbCubeFaceList.get(), m_cbCubeFaceList->GetCBVOffset(frameIndex));
#endif

	// Dispatch cube
	const auto gridSize = m_gridSize >> m_cubeMapLOD;
	pCommandList->Dispatch(XUSG_DIV_UP(gridSize, 8), XUSG_DIV_UP(gridSize, 8), m_cubeFaceCount);
}

void RayCaster::renderCube(CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	ResourceBarrier barriers[12];
	auto numBarriers = 0u;
	for (uint8_t i = 0; i < 6; ++i)
	{
		numBarriers = m_cubeMap->SetBarrier(barriers, m_cubeMapLOD, ResourceState::PIXEL_SHADER_RESOURCE, numBarriers, i);
		numBarriers = m_cubeDepth->SetBarrier(barriers, m_cubeMapLOD, ResourceState::PIXEL_SHADER_RESOURCE, numBarriers, i);
	}
	pCommandList->Barrier(numBarriers, barriers);

	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[RENDER_CUBE]);
	pCommandList->SetPipelineState(m_pipelines[RENDER_CUBE]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLESTRIP);

	// Set descriptor tables
	pCommandList->SetGraphicsDescriptorTable(0, m_cbvTables[frameIndex]);
	pCommandList->SetGraphicsDescriptorTable(1, m_srvMipTables[m_cubeMapLOD]);
	pCommandList->SetGraphicsDescriptorTable(2, m_srvTables[SRV_TABLE_DEPTH]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLESTRIP);
	pCommandList->Draw(4, 6, 0, 0);
}

void RayCaster::rayCastCube(CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	ResourceBarrier barriers[12];
	auto numBarriers = 0u;
	for (uint8_t i = 0; i < 6; ++i)
	{
		numBarriers = m_cubeMap->SetBarrier(barriers, m_cubeMapLOD, ResourceState::PIXEL_SHADER_RESOURCE, numBarriers, i);
		numBarriers = m_cubeDepth->SetBarrier(barriers, m_cubeMapLOD, ResourceState::PIXEL_SHADER_RESOURCE, numBarriers, i);
	}
	pCommandList->Barrier(numBarriers, barriers);

	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[RAY_CAST]);
	pCommandList->SetPipelineState(m_pipelines[RAY_CAST]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Set descriptor tables
	pCommandList->SetGraphicsDescriptorTable(0, m_cbvTables[frameIndex]);
	pCommandList->SetGraphicsDescriptorTable(1, m_srvMipTables[m_cubeMapLOD]);
	pCommandList->SetGraphicsDescriptorTable(2, m_srvTables[SRV_TABLE_DEPTH]);

	pCommandList->Draw(3, 1, 0, 0);
}

void RayCaster::rayCastDirect(CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	ResourceBarrier barrier;
	const auto numBarriers = m_pDepths[DEPTH_MAP]->SetBarrier(&barrier, ResourceState::PIXEL_SHADER_RESOURCE);
	pCommandList->Barrier(numBarriers, &barrier);

	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[DIRECT_RAY_CAST]);
	pCommandList->SetPipelineState(m_pipelines[DIRECT_RAY_CAST]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Set descriptor tables
	pCommandList->SetGraphicsDescriptorTable(0, m_cbvTables[frameIndex]);
	pCommandList->SetGraphicsDescriptorTable(1, m_srvTables[SRV_TABLE_VOLUME]);
	pCommandList->SetGraphicsDescriptorTable(2, m_srvTables[SRV_TABLE_DEPTH]);
	pCommandList->SetGraphics32BitConstant(3, m_maxRaySamples);
	pCommandList->SetGraphics32BitConstant(3, m_coeffSH ? 1 : 0, 1);
	pCommandList->SetGraphics32BitConstant(3, m_maxLightSamples, 2);
	if (m_coeffSH) pCommandList->SetGraphicsRootShaderResourceView(4, m_coeffSH.get());

	pCommandList->Draw(3, 1, 0, 0);
}

void RayCaster::rayCastVDirect(CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	ResourceBarrier barriers[2];
	auto numBarriers = m_lightMap->SetBarrier(barriers, ResourceState::PIXEL_SHADER_RESOURCE);
	numBarriers = m_pDepths[DEPTH_MAP]->SetBarrier(barriers, ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);

	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[DIRECT_RAY_CAST_V]);
	pCommandList->SetPipelineState(m_pipelines[DIRECT_RAY_CAST_V]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Set descriptor tables
	pCommandList->SetGraphicsDescriptorTable(0, m_cbvTables[frameIndex]);
	pCommandList->SetGraphicsDescriptorTable(1, m_srvTables[SRV_TABLE_VOLUME]);
	pCommandList->SetGraphicsDescriptorTable(2, m_srvTables[SRV_TABLE_DEPTH]);
	pCommandList->SetGraphics32BitConstant(3, m_maxRaySamples);

	pCommandList->Draw(3, 1, 0, 0);
}
