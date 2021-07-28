//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConsts.h"
#include "RayCaster.h"
#define _INDEPENDENT_DDS_LOADER_
#include "Advanced/XUSGDDSLoader.h"
#undef _INDEPENDENT_DDS_LOADER_

using namespace std;
using namespace DirectX;
using namespace XUSG;

struct CBPerObject
{
	XMFLOAT4X4 WorldViewProjI;
	XMFLOAT4X4 WorldViewProj;
	XMFLOAT4X4 ShadowWVP;
	XMFLOAT3X4 WorldI;
	XMFLOAT3X4 LightMapWorld;
	XMFLOAT4 EyePos;
	XMFLOAT4 LightPos;
	XMFLOAT4 LightColor;
	XMFLOAT4 Ambient;
};

#ifdef _CPU_SLICE_CULL_
static_assert(_CPU_SLICE_CULL_ == 0 || _CPU_SLICE_CULL_ == 1 || _CPU_SLICE_CULL_ == 2, "_CPU_SLICE_CULL_ can only be 0, 1, or 2");
#endif

#if _CPU_SLICE_CULL_
static inline bool IsSliceVisible(uint32_t slice, const float* localSpaceEyePt)
{
	const auto& viewComp = localSpaceEyePt[slice >> 1];

	return (slice & 0x1) ? viewComp > -1.0f : viewComp < 1.0f;
}
#endif

#if _CPU_SLICE_CULL_ == 1
static inline uint32_t GenVisibilityMask(CXMMATRIX worldI, const XMFLOAT3& eyePt)
{
	const auto localSpaceEyePt = XMVector3Transform(XMLoadFloat3(&eyePt), worldI);

	auto mask = 0u;
	for (uint8_t i = 0; i < 6; ++i)
	{
		const auto isVisible = IsSliceVisible(i, localSpaceEyePt.m128_f32);
		mask |= (isVisible ? 1 : 0) << i;
	}

	return mask;
}
#elif _CPU_SLICE_CULL_ == 2
struct CBSliceList
{
	XMUINT4 Slices[5];
};

static inline uint32_t GenVisibleSliceList(CBSliceList& sliceList, CXMMATRIX worldI, const XMFLOAT3& eyePt)
{
	const auto localSpaceEyePt = XMVector3Transform(XMLoadFloat3(&eyePt), worldI);

	auto count = 0u;
	for (uint8_t i = 0; i < 6; ++i)
	{
		if (IsSliceVisible(i, localSpaceEyePt.m128_f32))
		{
			assert(count < 5);
			sliceList.Slices[count++].x = i;
		}
	}

	return count;
}
#endif

RayCaster::RayCaster(const Device::sptr& device) :
	m_device(device),
	m_sliceCount(6),
	m_lightPt(75.0f, 75.0f, -75.0f),
	m_lightColor(1.0f, 0.7f, 0.3f, 1.0f),
	m_ambient(0.0f, 0.3f, 1.0f, 0.4f)
{
	m_shaderPool = ShaderPool::MakeUnique();
	m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(device.get());
	m_computePipelineCache = Compute::PipelineCache::MakeUnique(device.get());
	m_pipelineLayoutCache = PipelineLayoutCache::MakeUnique(device.get());

	XMStoreFloat3x4(&m_volumeWorld, XMMatrixScaling(10.0f, 10.0f, 10.0f));
	m_lightMapWorld = m_volumeWorld;
}

RayCaster::~RayCaster()
{
}

bool RayCaster::Init(const DescriptorTableCache::sptr& descriptorTableCache,
	Format rtFormat, uint32_t gridSize, const DepthStencil::uptr* depths)
{
	m_descriptorTableCache = descriptorTableCache;
	m_gridSize = gridSize;
	m_pDepths = depths;

	// Create resources
	m_volume = Texture3D::MakeUnique();
	N_RETURN(m_volume->Create(m_device.get(), gridSize, gridSize, gridSize, Format::R16G16B16A16_FLOAT,
		ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::ALLOW_SIMULTANEOUS_ACCESS, 1,
		MemoryType::DEFAULT, L"Volume"), false);

	m_cubeMap = Texture2D::MakeUnique();
	N_RETURN(m_cubeMap->Create(m_device.get(), gridSize, gridSize, Format::R8G8B8A8_UNORM, 6,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, MemoryType::DEFAULT, true, L"CubeMap"), false);

	m_cubeDepth = Texture2D::MakeUnique();
	N_RETURN(m_cubeDepth->Create(m_device.get(), gridSize, gridSize, Format::R32_FLOAT, 6,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, MemoryType::DEFAULT, true, L"CubeDepth"), false);

	m_lightGridSize = gridSize >> 1;
	m_lightMap = Texture3D::MakeUnique();
	N_RETURN(m_lightMap->Create(m_device.get(), m_lightGridSize, m_lightGridSize, m_lightGridSize,
		Format::R11G11B10_FLOAT,ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::ALLOW_SIMULTANEOUS_ACCESS,
		1, MemoryType::DEFAULT, L"LightMap"), false);

	m_cbPerObject = ConstantBuffer::MakeUnique();
	N_RETURN(m_cbPerObject->Create(m_device.get(), sizeof(CBPerObject[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, L"RayCaster.CBPerObject"), false);

#if _CPU_SLICE_CULL_ == 2
	m_cbSliceList = ConstantBuffer::MakeUnique();
	N_RETURN(m_cbSliceList->Create(m_device.get(), sizeof(CBSliceList[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, L"RayCaster.CBSliceList"), false);
#endif

	// Create pipelines
	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

bool RayCaster::LoadVolumeData(CommandList* pCommandList, const wchar_t* fileName, vector<Resource::uptr>& uploaders)
{
	// Load input image
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		uploaders.emplace_back(Resource::MakeUnique());
		N_RETURN(textureLoader.CreateTextureFromFile(m_device.get(), pCommandList, fileName,
			8192, false, m_fileSrc, uploaders.back().get(), &alphaMode), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_fileSrc->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_FILE_SRC], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	ResourceBarrier barrier;
	m_volume->SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);

	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	pCommandList->SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	// Set pipeline state
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[LOAD_VOLUME_DATA]);
	pCommandList->SetPipelineState(m_pipelines[LOAD_VOLUME_DATA]);

	// Set descriptor tables
	pCommandList->SetComputeDescriptorTable(0, m_srvTables[SRV_TABLE_FILE_SRC]);
	pCommandList->SetComputeDescriptorTable(1, m_uavTable);
	pCommandList->SetComputeDescriptorTable(2, m_samplerTable);

	// Dispatch grid
	pCommandList->Dispatch(DIV_UP(m_gridSize, 4), DIV_UP(m_gridSize, 4), DIV_UP(m_gridSize, 4));

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
	pCommandList->Dispatch(DIV_UP(m_gridSize, 4), DIV_UP(m_gridSize, 4), DIV_UP(m_gridSize, 4));
}

void RayCaster::SetVolumeWorld(float size, const DirectX::XMFLOAT3& pos)
{
	size *= 0.5f;
	auto world = XMMatrixScaling(size, size, size);
	world = world * XMMatrixTranslation(pos.x, pos.y, pos.z);
	XMStoreFloat3x4(&m_volumeWorld, world);
}

void RayCaster::SetLightMapWorld(float size, const DirectX::XMFLOAT3& pos)
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

void RayCaster::UpdateFrame(uint8_t frameIndex, CXMMATRIX viewProj, CXMMATRIX shadowVP, const XMFLOAT3& eyePt)
{
	// General matrices
	const auto world = XMLoadFloat3x4(&m_volumeWorld);
	const auto worldI = XMMatrixInverse(nullptr, world);
	const auto worldViewProj = world * viewProj;

	{
		// Screen space matrices
		const auto pCbData = reinterpret_cast<CBPerObject*>(m_cbPerObject->Map(frameIndex));
		XMStoreFloat4x4(&pCbData->WorldViewProj, XMMatrixTranspose(worldViewProj));
		XMStoreFloat4x4(&pCbData->WorldViewProjI, XMMatrixTranspose(XMMatrixInverse(nullptr, worldViewProj)));
		XMStoreFloat4x4(&pCbData->ShadowWVP, XMMatrixTranspose(world * shadowVP));
		XMStoreFloat3x4(&pCbData->WorldI, worldI);

		// Lighting
		pCbData->LightMapWorld = m_lightMapWorld;
		pCbData->EyePos = XMFLOAT4(eyePt.x, eyePt.y, eyePt.z, 1.0f);
		pCbData->LightPos = XMFLOAT4(m_lightPt.x, m_lightPt.y, m_lightPt.z, 1.0f);
		pCbData->LightColor = m_lightColor;
		pCbData->Ambient = m_ambient;
	}

#if _CPU_SLICE_CULL_ == 1
	m_visibilityMask = GenVisibilityMask(worldI, eyePt);
#elif _CPU_SLICE_CULL_ == 2
	{
		const auto pCbData = reinterpret_cast<CBSliceList*>(m_cbSliceList->Map(frameIndex));
		m_sliceCount = GenVisibleSliceList(*pCbData, worldI, eyePt);
	}
#endif
}

void RayCaster::Render(const CommandList* pCommandList, uint8_t frameIndex, uint8_t flags)
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
	pCommandList->SetComputeDescriptorTable(3, m_samplerTable);

	// Dispatch grid
	pCommandList->Dispatch(DIV_UP(m_lightGridSize, 4), DIV_UP(m_lightGridSize, 4), DIV_UP(m_lightGridSize, 4));
}

const DescriptorTable& RayCaster::GetVolumeSRVTable(const CommandList* pCommandList)
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
	// Load grid data
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(2, DescriptorType::SAMPLER, 1, 0);
		X_RETURN(m_pipelineLayouts[LOAD_VOLUME_DATA], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"LoadGridDataLayout"), false);
	}

	// Init grid data
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		X_RETURN(m_pipelineLayouts[INIT_VOLUME_DATA], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"InitGridDataLayout"), false);
	}

	// Ray marching
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::UAV, 2, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 2, 1);
		pipelineLayout->SetRange(3, DescriptorType::SAMPLER, 1, 0);
#if _CPU_SLICE_CULL_ == 1
		pipelineLayout->SetConstants(4, 1, 1);
#elif _CPU_SLICE_CULL_ == 2
		pipelineLayout->SetRootCBV(4, 1);
#endif
		X_RETURN(m_pipelineLayouts[RAY_MARCH], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"RayMarchingLayout"), false);
	}

	// Light space ray marching
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 1, 1);
		pipelineLayout->SetRange(3, DescriptorType::SAMPLER, 1, 0);
		X_RETURN(m_pipelineLayouts[RAY_MARCH_L], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"LightSpaceRayMarchingLayout"), false);
	}

	// View space ray marching
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::UAV, 2, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 2, 0);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 1, 2);
		pipelineLayout->SetRange(3, DescriptorType::SAMPLER, 1, 0);
#if _CPU_SLICE_CULL_ == 1
		pipelineLayout->SetConstants(4, 1, 1);
#elif _CPU_SLICE_CULL_ == 2
		pipelineLayout->SetRootCBV(4, 1);
#endif
		X_RETURN(m_pipelineLayouts[RAY_MARCH_V], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"ViewSpaceRayMarchingLayout"), false);
	}

	// Cube rendering
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 2, 0);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 1, 2);
		pipelineLayout->SetRange(3, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout->SetShaderStage(1, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(2, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(3, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[CUBE], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"CubeLayout"), false);
	}

	// Screen-space ray casting from cube map
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 2, 0);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 1, 2);
		pipelineLayout->SetRange(3, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(1, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(2, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(3, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[RAY_CAST], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"RayCastingLayout"), false);
	}

	// Direct ray casting
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 2, 1);
		pipelineLayout->SetRange(3, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(1, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(2, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(3, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[DIRECT_RAY_CAST], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"DirectRayCastingLayout"), false);
	}

	// View space direct ray casting
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 2, 0);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 1, 2);
		pipelineLayout->SetRange(3, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(1, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(2, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(2, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[DIRECT_RAY_CAST_V], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
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
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSR32FToRGBA16F.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[LOAD_VOLUME_DATA]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[LOAD_VOLUME_DATA], state->GetPipeline(m_computePipelineCache.get(), L"InitGridData"), false);
	}

	// Init grid data
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSInitGridData.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[INIT_VOLUME_DATA]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[INIT_VOLUME_DATA], state->GetPipeline(m_computePipelineCache.get(), L"InitGridData"), false);
	}

	// Ray marching
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSRayMarch.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_MARCH]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[RAY_MARCH], state->GetPipeline(m_computePipelineCache.get(), L"RayMarching"), false);
	}

	// Light space ray marching
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSRayMarchL.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_MARCH_L]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[RAY_MARCH_L], state->GetPipeline(m_computePipelineCache.get(), L"LightSpaceRayMarching"), false);
	}

	// View space ray marching
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSRayMarchV.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_MARCH_V]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[RAY_MARCH_V], state->GetPipeline(m_computePipelineCache.get(), L"ViewSpaceRayMarching"), false);
	}

	// Cube rendering
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSCube.cso"), false);
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSCube.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[CUBE]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->RSSetState(Graphics::CULL_FRONT, m_graphicsPipelineCache.get()); // Front-face culling for interior surfaces
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->OMSetBlendState(Graphics::PREMULTIPLITED, m_graphicsPipelineCache.get());
		state->OMSetRTVFormats(&rtFormat, 1);
		X_RETURN(m_pipelines[CUBE], state->GetPipeline(m_graphicsPipelineCache.get(), L"RayCasting"), false);
	}

	N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);

	// Screen-space ray casting from cube map
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSRayCastCube.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_CAST]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->OMSetBlendState(Graphics::PREMULTIPLITED, m_graphicsPipelineCache.get());
		state->OMSetRTVFormats(&rtFormat, 1);
		X_RETURN(m_pipelines[RAY_CAST], state->GetPipeline(m_graphicsPipelineCache.get(), L"RayCasting"), false);
	}

	// Direct Ray casting
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSRayCast.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[DIRECT_RAY_CAST]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->OMSetBlendState(Graphics::PREMULTIPLITED, m_graphicsPipelineCache.get());
		state->OMSetRTVFormats(&rtFormat, 1);
		X_RETURN(m_pipelines[DIRECT_RAY_CAST], state->GetPipeline(m_graphicsPipelineCache.get(), L"DirectRayCasting"), false);
	}

	// View space direct Ray casting
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSRayCastV.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[DIRECT_RAY_CAST_V]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->OMSetBlendState(Graphics::PREMULTIPLITED, m_graphicsPipelineCache.get());
		state->OMSetRTVFormats(&rtFormat, 1);
		X_RETURN(m_pipelines[DIRECT_RAY_CAST_V], state->GetPipeline(m_graphicsPipelineCache.get(), L"ViewSpaceDirectRayCasting"), false);
	}

	return true;
}

bool RayCaster::createDescriptorTables()
{
	// Create CBV tables
	for (uint8_t i = 0; i < FrameCount; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_cbPerObject->GetCBV(i));
		X_RETURN(m_cbvTables[i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create UAV and SRV table
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const Descriptor descriptors[] =
		{
			m_cubeMap->GetUAV(),
			m_cubeDepth->GetUAV()
			//m_volume->GetSRV(),	// shared with m_srvTables[SRV_TABLE_VOLUME]
			//m_lightMap->GetSRV()	// shared with m_srvTables[SRV_TABLE_LIGHT_MAP]
		};
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_uavSrvTable, descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create SRV tables
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_volume->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_VOLUME], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_lightMap->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_LIGHT_MAP], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
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
		X_RETURN(m_srvUavTable, descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create SRV tables
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const Descriptor descriptors[] =
		{
			m_cubeMap->GetSRV(),
			m_cubeDepth->GetSRV()
		};
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_CUBE_MAP], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	if (m_pDepths[DEPTH_MAP])
	{
		{
			const auto descriptorTable = Util::DescriptorTable::MakeUnique();
			descriptorTable->SetDescriptors(0, 1, &m_pDepths[DEPTH_MAP]->GetSRV());
			X_RETURN(m_srvTables[SRV_TABLE_DEPTH], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
		}

		{
			const auto descriptorTable = Util::DescriptorTable::MakeUnique();
			descriptorTable->SetDescriptors(0, 1, &m_pDepths[SHADOW_MAP]->GetSRV());
			X_RETURN(m_srvTables[SRV_TABLE_SHADOW], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
		}
	}

	// Create UAV table
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_volume->GetUAV());
		X_RETURN(m_uavTable, descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create the sampler
	const auto descriptorTable = Util::DescriptorTable::MakeUnique();
	const auto samplerLinearClamp = SamplerPreset::LINEAR_CLAMP;
	descriptorTable->SetSamplers(0, 1, &samplerLinearClamp, m_descriptorTableCache.get());
	X_RETURN(m_samplerTable, descriptorTable->GetSamplerTable(m_descriptorTableCache.get()), false);

	return true;
}

void RayCaster::rayMarch(const CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	ResourceBarrier barriers[2];
	auto numBarriers = m_cubeMap->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS);
	numBarriers = m_pDepths[DEPTH_MAP]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);

	// Set pipeline state
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[RAY_MARCH]);
	pCommandList->SetPipelineState(m_pipelines[RAY_MARCH]);

	// Set descriptor tables
	pCommandList->SetComputeDescriptorTable(0, m_cbvTables[frameIndex]);
	pCommandList->SetComputeDescriptorTable(1, m_uavSrvTable);
	pCommandList->SetComputeDescriptorTable(2, m_srvTables[SRV_TABLE_DEPTH]);
	pCommandList->SetComputeDescriptorTable(3, m_samplerTable);
#if _CPU_SLICE_CULL_ == 1
	pCommandList->SetCompute32BitConstant(4, m_visibilityMask);
#elif _CPU_SLICE_CULL_ == 2
	pCommandList->SetComputeRootConstantBufferView(4, m_cbSliceList.get(), m_cbSliceList->GetCBVOffset(frameIndex));
#endif

	// Dispatch cube
	pCommandList->Dispatch(DIV_UP(m_gridSize, 8), DIV_UP(m_gridSize, 8), m_sliceCount);
}

void RayCaster::rayMarchV(const CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	ResourceBarrier barriers[3];
	auto numBarriers = m_lightMap->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE);
	numBarriers = m_pDepths[DEPTH_MAP]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	numBarriers = m_cubeMap->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);

	// Set pipeline state
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[RAY_MARCH_V]);
	pCommandList->SetPipelineState(m_pipelines[RAY_MARCH_V]);

	// Set descriptor tables
	pCommandList->SetComputeDescriptorTable(0, m_cbvTables[frameIndex]);
	pCommandList->SetComputeDescriptorTable(1, m_uavSrvTable);
	pCommandList->SetComputeDescriptorTable(2, m_srvTables[SRV_TABLE_DEPTH]);
	pCommandList->SetComputeDescriptorTable(3, m_samplerTable);
#if _CPU_SLICE_CULL_ == 1
	pCommandList->SetCompute32BitConstant(4, m_visibilityMask);
#elif _CPU_SLICE_CULL_ == 2
	pCommandList->SetComputeRootConstantBufferView(4, m_cbSliceList.get(), m_cbSliceList->GetCBVOffset(frameIndex));
#endif

	// Dispatch cube
	pCommandList->Dispatch(DIV_UP(m_gridSize, 8), DIV_UP(m_gridSize, 8), m_sliceCount);
}

void RayCaster::renderCube(const CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	ResourceBarrier barrier;
	const auto numBarriers = m_cubeMap->SetBarrier(&barrier, ResourceState::PIXEL_SHADER_RESOURCE);
	pCommandList->Barrier(numBarriers, &barrier);

	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[CUBE]);
	pCommandList->SetPipelineState(m_pipelines[CUBE]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLESTRIP);

	// Set descriptor tables
	pCommandList->SetGraphicsDescriptorTable(0, m_cbvTables[frameIndex]);
	pCommandList->SetGraphicsDescriptorTable(1, m_srvTables[SRV_TABLE_CUBE_MAP]);
	pCommandList->SetGraphicsDescriptorTable(2, m_srvTables[SRV_TABLE_DEPTH]);
	pCommandList->SetGraphicsDescriptorTable(3, m_samplerTable);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLESTRIP);
	pCommandList->Draw(4, 6, 0, 0);
}

void RayCaster::rayCastCube(const CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	ResourceBarrier barrier;
	const auto numBarriers = m_cubeMap->SetBarrier(&barrier, ResourceState::PIXEL_SHADER_RESOURCE);
	pCommandList->Barrier(numBarriers, &barrier);

	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[RAY_CAST]);
	pCommandList->SetPipelineState(m_pipelines[RAY_CAST]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Set descriptor tables
	pCommandList->SetGraphicsDescriptorTable(0, m_cbvTables[frameIndex]);
	pCommandList->SetGraphicsDescriptorTable(1, m_srvTables[SRV_TABLE_CUBE_MAP]);
	pCommandList->SetGraphicsDescriptorTable(2, m_srvTables[SRV_TABLE_DEPTH]);
	pCommandList->SetGraphicsDescriptorTable(3, m_samplerTable);

	pCommandList->Draw(3, 1, 0, 0);
}

void RayCaster::rayCastDirect(const CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	ResourceBarrier barrier;
	const auto numBarriers = m_pDepths[DEPTH_MAP]->SetBarrier(&barrier, ResourceState::NON_PIXEL_SHADER_RESOURCE);
	pCommandList->Barrier(numBarriers, &barrier);

	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[DIRECT_RAY_CAST]);
	pCommandList->SetPipelineState(m_pipelines[DIRECT_RAY_CAST]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Set descriptor tables
	pCommandList->SetGraphicsDescriptorTable(0, m_cbvTables[frameIndex]);
	pCommandList->SetGraphicsDescriptorTable(1, m_srvTables[SRV_TABLE_VOLUME]);
	pCommandList->SetGraphicsDescriptorTable(2, m_srvTables[SRV_TABLE_DEPTH]);
	pCommandList->SetGraphicsDescriptorTable(3, m_samplerTable);

	pCommandList->Draw(3, 1, 0, 0);
}

void RayCaster::rayCastVDirect(const CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	ResourceBarrier barriers[2];
	auto numBarriers = m_lightMap->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE);
	numBarriers = m_pDepths[DEPTH_MAP]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);

	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[DIRECT_RAY_CAST_V]);
	pCommandList->SetPipelineState(m_pipelines[DIRECT_RAY_CAST_V]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Set descriptor tables
	pCommandList->SetGraphicsDescriptorTable(0, m_cbvTables[frameIndex]);
	pCommandList->SetGraphicsDescriptorTable(1, m_srvTables[SRV_TABLE_VOLUME]);
	pCommandList->SetGraphicsDescriptorTable(2, m_srvTables[SRV_TABLE_DEPTH]);
	pCommandList->SetGraphicsDescriptorTable(3, m_samplerTable);

	pCommandList->Draw(3, 1, 0, 0);
}
