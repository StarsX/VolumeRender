//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "RayCaster.h"
#define _INDEPENDENT_DDS_LOADER_
#include "Advanced/XUSGDDSLoader.h"
#undef _INDEPENDENT_DDS_LOADER_

using namespace std;
using namespace DirectX;
using namespace XUSG;

struct CBPerObject
{
	XMMATRIX WorldViewProjI;
	XMMATRIX WorldI;
	XMMATRIX LightMapWorld;
	XMFLOAT4 EyePos;
	XMFLOAT4 LightPos;
	XMFLOAT4 LightColor;
	XMFLOAT4 Ambient;
};

RayCaster::RayCaster(const Device& device) :
	m_device(device),
	m_lightPt(75.0f, 75.0f, -75.0f),
	m_lightColor(1.0f, 0.7f, 0.3f, 1.0f),
	m_ambient(0.0f, 0.3f, 1.0f, 0.3f)
{
	m_shaderPool = ShaderPool::MakeUnique();
	m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(device);
	m_computePipelineCache = Compute::PipelineCache::MakeUnique(device);
	m_pipelineLayoutCache = PipelineLayoutCache::MakeUnique(device);

	XMStoreFloat4x4(&m_volumeWorld, XMMatrixScaling(10.0f, 10.0f, 10.0f));
	m_lightMapWorld = m_volumeWorld;
}

RayCaster::~RayCaster()
{
}

bool RayCaster::Init(uint32_t width, uint32_t height, DescriptorTableCache::sptr descriptorTableCache,
	Format rtFormat, Format dsFormat, uint32_t gridSize)
{
	m_viewport = XMUINT2(width, height);
	m_descriptorTableCache = descriptorTableCache;
	m_gridSize = gridSize;

	// Create resources
	m_volume = Texture3D::MakeUnique();
	N_RETURN(m_volume->Create(m_device, gridSize, gridSize, gridSize, Format::R16G16B16A16_FLOAT,
		ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::ALLOW_SIMULTANEOUS_ACCESS, 1,
		MemoryType::DEFAULT, L"Volume"), false);

	m_cubeMap = Texture2D::MakeUnique();
	N_RETURN(m_cubeMap->Create(m_device, gridSize, gridSize, Format::R8G8B8A8_UNORM, 6,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, MemoryType::DEFAULT, true, L"CubeMap"), false);

	m_lightGridSize = gridSize >> 1;
	m_lightMap = Texture3D::MakeUnique();
	N_RETURN(m_lightMap->Create(m_device, m_lightGridSize, m_lightGridSize, m_lightGridSize,
		Format::R11G11B10_FLOAT,ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::ALLOW_SIMULTANEOUS_ACCESS,
		1, MemoryType::DEFAULT, L"LightMap"), false);

	m_cbPerObject = ConstantBuffer::MakeUnique();
	N_RETURN(m_cbPerObject->Create(m_device, sizeof(CBPerObject), FrameCount,
		nullptr, MemoryType::UPLOAD, L"CBPerObject"), false);

	// Create pipelines
	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat, dsFormat), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

bool RayCaster::LoadVolumeData(CommandList* pCommandList, const wchar_t* fileName, vector<Resource>& uploaders)
{
	// Load input image
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		uploaders.push_back(nullptr);
		N_RETURN(textureLoader.CreateTextureFromFile(m_device, pCommandList, fileName,
			8192, false, m_fileSrc, uploaders.back(), &alphaMode), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_fileSrc->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_FILE_SRC], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
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
	XMStoreFloat4x4(&m_volumeWorld, world);
}

void RayCaster::SetLightMapWorld(float size, const DirectX::XMFLOAT3& pos)
{
	size *= 0.5f;
	auto world = XMMatrixScaling(size, size, size);
	world = world * XMMatrixTranslation(pos.x, pos.y, pos.z);
	XMStoreFloat4x4(&m_lightMapWorld, world);
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

void RayCaster::UpdateFrame(uint32_t frameIndex, CXMMATRIX viewProj, const XMFLOAT3& eyePt)
{
	// General matrices
	const auto world = XMLoadFloat4x4(&m_volumeWorld);
	const auto worldViewProj = world * viewProj;

	// Screen space matrices
	const auto pCbPerObject = reinterpret_cast<CBPerObject*>(m_cbPerObject->Map(frameIndex));
	pCbPerObject->WorldViewProjI = XMMatrixTranspose(XMMatrixInverse(nullptr, worldViewProj));
	pCbPerObject->WorldI = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

	// Lighting
	pCbPerObject->LightMapWorld = XMMatrixTranspose(XMLoadFloat4x4(&m_lightMapWorld));
	pCbPerObject->EyePos = XMFLOAT4(eyePt.x, eyePt.y, eyePt.z, 1.0f);
	pCbPerObject->LightPos = XMFLOAT4(m_lightPt.x, m_lightPt.y, m_lightPt.z, 1.0f);
	pCbPerObject->LightColor = m_lightColor;
	pCbPerObject->Ambient = m_ambient;
}

void RayCaster::Render(const CommandList* pCommandList, uint32_t frameIndex, bool splitLightPass)
{
	if (splitLightPass)
	{
		RayMarchL(pCommandList, frameIndex);
		rayMarchV(pCommandList, frameIndex);
	}
	else rayMarch(pCommandList, frameIndex);

	rayCast(pCommandList, frameIndex);
}

void RayCaster::RayMarchL(const CommandList* pCommandList, uint32_t frameIndex)
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
	pCommandList->SetComputeDescriptorTable(2, m_samplerTable);

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

ResourceBase& RayCaster::GetLightMap()
{
	return *m_lightMap;
}

bool RayCaster::createPipelineLayouts()
{
	// Load grid data
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(2, DescriptorType::SAMPLER, 1, 0);
		X_RETURN(m_pipelineLayouts[LOAD_VOLUME_DATA], pipelineLayout->GetPipelineLayout(*m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"LoadGridDataLayout"), false);
	}

	// Init grid data
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		X_RETURN(m_pipelineLayouts[INIT_VOLUME_DATA], pipelineLayout->GetPipelineLayout(*m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"InitGridDataLayout"), false);
	}

	// Ray marching
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetRange(2, DescriptorType::SAMPLER, 1, 0);
		X_RETURN(m_pipelineLayouts[RAY_MARCH], pipelineLayout->GetPipelineLayout(*m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"RayMarchingLayout"), false);
	}

	// Light space ray marching
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(2, DescriptorType::SAMPLER, 1, 0);
		X_RETURN(m_pipelineLayouts[RAY_MARCH_L], pipelineLayout->GetPipelineLayout(*m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"LightSpaceRayMarchingLayout"), false);
	}

	// View space ray marching
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 2, 0);
		pipelineLayout->SetRange(2, DescriptorType::SAMPLER, 1, 0);
		X_RETURN(m_pipelineLayouts[RAY_MARCH_V], pipelineLayout->GetPipelineLayout(*m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"ViewSpaceRayMarchingLayout"), false);
	}

	// Ray casting
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetRange(2, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(1, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(2, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[RAY_CAST], pipelineLayout->GetPipelineLayout(*m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"RayCastingLayout"), false);
	}

	return true;
}

bool RayCaster::createPipelines(Format rtFormat, Format dsFormat)
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
		X_RETURN(m_pipelines[LOAD_VOLUME_DATA], state->GetPipeline(*m_computePipelineCache, L"InitGridData"), false);
	}

	// Init grid data
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSInitGridData.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[INIT_VOLUME_DATA]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[INIT_VOLUME_DATA], state->GetPipeline(*m_computePipelineCache, L"InitGridData"), false);
	}

	// Ray marching
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSRayMarch.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_MARCH]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[RAY_MARCH], state->GetPipeline(*m_computePipelineCache, L"RayMarching"), false);
	}

	// Light space ray marching
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSRayMarchL.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_MARCH_L]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[RAY_MARCH_L], state->GetPipeline(*m_computePipelineCache, L"LightSpaceRayMarching"), false);
	}

	// View space ray marching
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSRayMarchV.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_MARCH_V]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[RAY_MARCH_V], state->GetPipeline(*m_computePipelineCache, L"ViewSpaceRayMarching"), false);
	}

	// Ray casting
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSRayCast.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_CAST]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, *m_graphicsPipelineCache);
		//state->OMSetBlendState(Graphics::NON_PRE_MUL, *m_graphicsPipelineCache);
		state->OMSetBlendState(Graphics::PREMULTIPLITED, *m_graphicsPipelineCache);
		state->OMSetRTVFormats(&rtFormat, 1);
		X_RETURN(m_pipelines[RAY_CAST], state->GetPipeline(*m_graphicsPipelineCache, L"RayCasting"), false);
	}

	return true;
}

bool RayCaster::createDescriptorTables()
{
	m_descriptorTableCache->AllocateDescriptorPool(CBV_SRV_UAV_POOL, 28);

	// Create CBV tables
	for (auto i = 0u; i < FrameCount; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_cbPerObject->GetCBV(i));
		X_RETURN(m_cbvTables[i], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create UAV and SRV table
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const Descriptor descriptors[] =
		{
			m_cubeMap->GetUAV(),
			m_volume->GetSRV(),
			m_lightMap->GetSRV()
		};
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_uavSrvTable, descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create SRV table
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_lightMap->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_LIGHT_MAP], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
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
		X_RETURN(m_srvUavTable, descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create SRV table
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_cubeMap->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_CUBE_MAP], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create UAV table
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_volume->GetUAV());
		X_RETURN(m_uavTable, descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create the sampler
	const auto descriptorTable = Util::DescriptorTable::MakeUnique();
	const auto samplerLinearClamp = SamplerPreset::LINEAR_CLAMP;
	descriptorTable->SetSamplers(0, 1, &samplerLinearClamp, *m_descriptorTableCache);
	X_RETURN(m_samplerTable, descriptorTable->GetSamplerTable(*m_descriptorTableCache), false);

	return true;
}

void RayCaster::rayMarch(const CommandList* pCommandList, uint32_t frameIndex)
{
	// Set barriers
	ResourceBarrier barrier;
	const auto numBarriers = m_cubeMap->SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);
	pCommandList->Barrier(numBarriers, &barrier);

	// Set pipeline state
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[RAY_MARCH]);
	pCommandList->SetPipelineState(m_pipelines[RAY_MARCH]);

	// Set descriptor tables
	pCommandList->SetComputeDescriptorTable(0, m_cbvTables[frameIndex]);
	pCommandList->SetComputeDescriptorTable(1, m_uavSrvTable);
	pCommandList->SetComputeDescriptorTable(2, m_samplerTable);

	// Dispatch halved cube
	pCommandList->Dispatch(DIV_UP(m_gridSize, 8), DIV_UP(m_gridSize, 8), 6);
}

void RayCaster::rayMarchV(const CommandList* pCommandList, uint32_t frameIndex)
{
	// Set barriers
	ResourceBarrier barriers[2];
	auto numBarriers = m_lightMap->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE);
	numBarriers = m_cubeMap->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);

	// Set pipeline state
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[RAY_MARCH_V]);
	pCommandList->SetPipelineState(m_pipelines[RAY_MARCH_V]);

	// Set descriptor tables
	pCommandList->SetComputeDescriptorTable(0, m_cbvTables[frameIndex]);
	pCommandList->SetComputeDescriptorTable(1, m_uavSrvTable);
	pCommandList->SetComputeDescriptorTable(2, m_samplerTable);

	// Dispatch halved cube
	pCommandList->Dispatch(DIV_UP(m_gridSize, 8), DIV_UP(m_gridSize, 8), 6);
}

void RayCaster::rayCast(const CommandList* pCommandList, uint32_t frameIndex)
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
	pCommandList->SetGraphicsDescriptorTable(2, m_samplerTable);

	pCommandList->Draw(3, 1, 0, 0);
}
