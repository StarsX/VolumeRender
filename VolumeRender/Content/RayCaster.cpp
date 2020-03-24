//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "RayCaster.h"

using namespace std;
using namespace DirectX;
using namespace XUSG;

struct CBPerObject
{
	XMMATRIX WorldViewProj;
	XMMATRIX WorldViewProjI;
	XMVECTOR LocalSpaceEyePt;
	XMVECTOR LocalSpaceLightPt;
	XMFLOAT4 LightColor;
	XMFLOAT4 Ambient;
};

RayCaster::RayCaster(const Device& device) :
	m_device(device),
	m_lightPt(75.0f, 75.0f, -75.0f),
	m_lightColor(1.0f, 0.7f, 0.3f, 1.0f),
	m_ambient(0.0f, 0.3f, 1.0f, 0.3f)
{
	m_graphicsPipelineCache.SetDevice(device);
	m_computePipelineCache.SetDevice(device);
	m_pipelineLayoutCache.SetDevice(device);

	XMStoreFloat4x4(&m_world, XMMatrixScaling(10.0f, 10.0f, 10.0f));
}

RayCaster::~RayCaster()
{
}

bool RayCaster::Init(const CommandList& commandList, uint32_t width, uint32_t height,
	shared_ptr<DescriptorTableCache> descriptorTableCache, vector<Resource>& uploaders,
	Format rtFormat, Format dsFormat, const XMUINT3& gridSize)
{
	m_viewport = XMUINT2(width, height);
	m_descriptorTableCache = descriptorTableCache;
	m_gridSize = gridSize;

	// Create resources
	N_RETURN(m_grid.Create(m_device, gridSize.x, gridSize.y, gridSize.z, Format::R16G16B16A16_FLOAT,
		ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::ALLOW_SIMULTANEOUS_ACCESS, 1,
		MemoryType::DEFAULT, L"Grid"), false);

	N_RETURN(m_halvedCube[0].Create(m_device, gridSize.z, gridSize.y, Format::R8G8B8A8_UNORM, 1,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, MemoryType::DEFAULT, false, L"HalvedCubeX"), false);
	N_RETURN(m_halvedCube[1].Create(m_device, gridSize.x, gridSize.z, Format::R8G8B8A8_UNORM, 1,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, MemoryType::DEFAULT, false, L"HalvedCubeY"), false);
	N_RETURN(m_halvedCube[2].Create(m_device, gridSize.x, gridSize.y, Format::R8G8B8A8_UNORM, 1,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, MemoryType::DEFAULT, false, L"HalvedCubeZ"), false);

	m_lightGridSize = XMUINT3(gridSize.x >> 1, gridSize.y >> 1, gridSize.z >> 1);
	N_RETURN(m_lightMap.Create(m_device, m_lightGridSize.x, m_lightGridSize.y, m_lightGridSize.z,
		Format::R11G11B10_FLOAT,ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::ALLOW_SIMULTANEOUS_ACCESS,
		1, MemoryType::DEFAULT, L"Light"), false);

	N_RETURN(m_cbPerObject.Create(m_device, sizeof(CBPerObject), FrameCount,
		nullptr, MemoryType::UPLOAD, L"CBPerObject"), false);

	// Create pipelines
	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat, dsFormat), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

void RayCaster::InitGridData(const CommandList& commandList)
{
	const DescriptorPool pDescriptorPool[] =
	{ m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL) };
	commandList.SetDescriptorPools(1, pDescriptorPool);

	// Set pipeline state
	commandList.SetComputePipelineLayout(m_pipelineLayouts[INIT_GRID_DATA]);
	commandList.SetPipelineState(m_pipelines[INIT_GRID_DATA]);

	// Set descriptor tables
	commandList.SetComputeDescriptorTable(0, m_uavTable);

	// Dispatch grid
	commandList.Dispatch(DIV_UP(m_gridSize.x, 4), DIV_UP(m_gridSize.y, 4), DIV_UP(m_gridSize.z, 4));
}

void RayCaster::SetWorld(float size, const DirectX::XMFLOAT3& pos)
{
	size *= 0.5f;
	auto world = XMMatrixScaling(size, size, size);
	world = world * XMMatrixTranslation(pos.x, pos.y, pos.z);
	XMStoreFloat4x4(&m_world, world);
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

void RayCaster::UpdateFrame(uint32_t frameIndex, CXMMATRIX viewProj, CXMVECTOR eyePt)
{
	// General matrices
	const auto world = XMLoadFloat4x4(&m_world);
	const auto worldViewProj = world * viewProj;
	const auto worldI = XMMatrixInverse(nullptr, world);
	const auto worldViewProjI = XMMatrixInverse(nullptr, worldViewProj);

	// Screen space matrices
	const auto pCbPerObject = reinterpret_cast<CBPerObject*>(m_cbPerObject.Map(frameIndex));
	pCbPerObject->WorldViewProj = XMMatrixTranspose(worldViewProj);
	pCbPerObject->WorldViewProjI = XMMatrixTranspose(worldViewProjI);
	pCbPerObject->LocalSpaceEyePt = XMVector3TransformCoord(eyePt, worldI);
	pCbPerObject->LocalSpaceLightPt = XMVector3TransformNormal(XMLoadFloat3(&m_lightPt), worldI);
	pCbPerObject->LightColor = m_lightColor;
	pCbPerObject->Ambient = m_ambient;
}

void RayCaster::Render(const CommandList& commandList, uint32_t frameIndex, bool splitLightPass)
{
	if (splitLightPass)
	{
		RayMarchL(commandList, frameIndex);
		rayMarchV(commandList, frameIndex);
	}
	else rayMarch(commandList, frameIndex);

	rayCast(commandList, frameIndex);
}

void RayCaster::RayMarchL(const CommandList& commandList, uint32_t frameIndex)
{
	// Set barrier
	ResourceBarrier barrier;
	m_lightMap.SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);

	// Set pipeline state
	commandList.SetComputePipelineLayout(m_pipelineLayouts[RAY_MARCH_L]);
	commandList.SetPipelineState(m_pipelines[RAY_MARCH_L]);

	// Set descriptor tables
	commandList.SetComputeDescriptorTable(0, m_cbvTables[frameIndex]);
	commandList.SetComputeDescriptorTable(1, m_srvUavTable);
	commandList.SetComputeDescriptorTable(2, m_samplerTable);

	// Dispatch grid
	commandList.Dispatch(DIV_UP(m_lightGridSize.x, 4), DIV_UP(m_lightGridSize.y, 4), DIV_UP(m_lightGridSize.z, 4));
}

const DescriptorTable& RayCaster::GetGridSRVTable(const CommandList& commandList)
{
	// Set barrier
	ResourceBarrier barrier;
	const auto numBarriers = m_grid.SetBarrier(&barrier, ResourceState::NON_PIXEL_SHADER_RESOURCE |
		ResourceState::PIXEL_SHADER_RESOURCE);
	commandList.Barrier(numBarriers, &barrier);

	return m_srvUavTable;
}

const DescriptorTable& RayCaster::GetLightSRVTable() const
{
	return m_srvTables[SRV_TABLE_LIGHT_MAP];
}

ResourceBase& RayCaster::GetLightMap()
{
	return m_lightMap;
}

bool RayCaster::createPipelineLayouts()
{
	// Init grid data
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(0, DescriptorType::UAV, 1, 0, 0, DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		X_RETURN(m_pipelineLayouts[INIT_GRID_DATA], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"InitGridDataLayout"), false);
	}

	// Ray marching
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(0, DescriptorType::CBV, 1, 0);
		pipelineLayout.SetRange(1, DescriptorType::UAV, 3, 0, 0, DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		pipelineLayout.SetRange(2, DescriptorType::SAMPLER, 1, 0);
		X_RETURN(m_pipelineLayouts[RAY_MARCH], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"RayMarchingLayout"), false);
	}

	// Light space ray marching
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(0, DescriptorType::CBV, 1, 0);
		pipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		pipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout.SetRange(2, DescriptorType::SAMPLER, 1, 0);
		X_RETURN(m_pipelineLayouts[RAY_MARCH_L], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"LightSpaceRayMarchingLayout"), false);
	}

	// View space ray marching
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(0, DescriptorType::CBV, 1, 0);
		pipelineLayout.SetRange(1, DescriptorType::UAV, 3, 0, 0, DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout.SetRange(1, DescriptorType::SRV, 2, 0);
		pipelineLayout.SetRange(2, DescriptorType::SAMPLER, 1, 0);
		X_RETURN(m_pipelineLayouts[RAY_MARCH_V], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"ViewSpaceRayMarchingLayout"), false);
	}

	// Ray casting
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(0, DescriptorType::CBV, 1, 0);
		pipelineLayout.SetRange(1, DescriptorType::SRV, 3, 0);
		pipelineLayout.SetRange(2, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout.SetShaderStage(0, Shader::Stage::PS);
		pipelineLayout.SetShaderStage(1, Shader::Stage::PS);
		pipelineLayout.SetShaderStage(2, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[RAY_CAST], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"RayCastingLayout"), false);
	}

	return true;
}

bool RayCaster::createPipelines(Format rtFormat, Format dsFormat)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	// Init grid data
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, csIndex, L"CSInitGridData.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[INIT_GRID_DATA]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[INIT_GRID_DATA], state.GetPipeline(m_computePipelineCache, L"InitGridData"), false);
	}

	// Ray marching
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, csIndex, L"CSRayMarch.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RAY_MARCH]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[RAY_MARCH], state.GetPipeline(m_computePipelineCache, L"RayMarching"), false);
	}

	// Light space ray marching
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, csIndex, L"CSRayMarchL.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RAY_MARCH_L]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[RAY_MARCH_L], state.GetPipeline(m_computePipelineCache, L"LightSpaceRayMarching"), false);
	}

	// View space ray marching
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, csIndex, L"CSRayMarchV.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RAY_MARCH_V]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[RAY_MARCH_V], state.GetPipeline(m_computePipelineCache, L"ViewSpaceRayMarching"), false);
	}

	// Ray casting
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::PS, psIndex, L"PSRayCast.cso"), false);

		Graphics::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RAY_CAST]);
		state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, vsIndex++));
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, psIndex++));
		state.IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state.DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache);
		//state.OMSetBlendState(Graphics::NON_PRE_MUL, m_graphicsPipelineCache);
		state.OMSetBlendState(Graphics::PREMULTIPLITED, m_graphicsPipelineCache);
		state.OMSetRTVFormats(&rtFormat, 1);
		X_RETURN(m_pipelines[RAY_CAST], state.GetPipeline(m_graphicsPipelineCache, L"RayCasting"), false);
	}

	return true;
}

bool RayCaster::createDescriptorTables()
{
	// Create CBV tables
	for (auto i = 0u; i < FrameCount; ++i)
	{
		Util::DescriptorTable cbvTable;
		cbvTable.SetDescriptors(0, 1, &m_cbPerObject.GetCBV(i));
		X_RETURN(m_cbvTables[i], cbvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create UAV and SRV table
	{
		Util::DescriptorTable uavSrvTable;
		const Descriptor descriptors[] =
		{
			m_halvedCube[0].GetUAV(),
			m_halvedCube[1].GetUAV(),
			m_halvedCube[2].GetUAV(),
			m_grid.GetSRV(),
			m_lightMap.GetSRV()
		};
		uavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_uavSrvTable, uavSrvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create SRV table
	{
		Util::DescriptorTable srvTable;
		srvTable.SetDescriptors(0, 1, &m_lightMap.GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_LIGHT_MAP], srvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create SRV and UAV table
	{
		Util::DescriptorTable srvUavTable;
		const Descriptor descriptors[] =
		{
			m_grid.GetSRV(),
			m_lightMap.GetUAV()
		};
		srvUavTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvUavTable, srvUavTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create SRV table
	{
		Util::DescriptorTable srvTable;
		const Descriptor descriptors[] =
		{
			m_halvedCube[0].GetSRV(),
			m_halvedCube[1].GetSRV(),
			m_halvedCube[2].GetSRV()
		};
		srvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_HALVED_CUBE], srvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create UAV table
	{
		Util::DescriptorTable uavTable;
		uavTable.SetDescriptors(0, 1, &m_grid.GetUAV());
		X_RETURN(m_uavTable, uavTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create the sampler
	Util::DescriptorTable samplerTable;
	const auto samplerLinearClamp = SamplerPreset::LINEAR_CLAMP;
	samplerTable.SetSamplers(0, 1, &samplerLinearClamp, *m_descriptorTableCache);
	X_RETURN(m_samplerTable, samplerTable.GetSamplerTable(*m_descriptorTableCache), false);

	return true;
}

void RayCaster::rayMarch(const CommandList& commandList, uint32_t frameIndex)
{
	// Set barriers
	ResourceBarrier barriers[3];
	auto numBarriers = 0u;
	for (auto& slice : m_halvedCube)
		numBarriers = slice.SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, numBarriers);
	commandList.Barrier(numBarriers, barriers);

	// Set pipeline state
	commandList.SetComputePipelineLayout(m_pipelineLayouts[RAY_MARCH]);
	commandList.SetPipelineState(m_pipelines[RAY_MARCH]);

	// Set descriptor tables
	commandList.SetComputeDescriptorTable(0, m_cbvTables[frameIndex]);
	commandList.SetComputeDescriptorTable(1, m_uavSrvTable);
	commandList.SetComputeDescriptorTable(2, m_samplerTable);

	// Dispatch halved cube
	commandList.Dispatch(DIV_UP(m_gridSize.x, 8), DIV_UP(m_gridSize.y, 8), 3);
}

void RayCaster::rayMarchV(const CommandList& commandList, uint32_t frameIndex)
{
	// Set barriers
	ResourceBarrier barriers[4];
	auto numBarriers = m_lightMap.SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE);
	for (auto& slice : m_halvedCube)
		numBarriers = slice.SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, numBarriers);
	commandList.Barrier(numBarriers, barriers);

	// Set pipeline state
	commandList.SetComputePipelineLayout(m_pipelineLayouts[RAY_MARCH_V]);
	commandList.SetPipelineState(m_pipelines[RAY_MARCH_V]);

	// Set descriptor tables
	commandList.SetComputeDescriptorTable(0, m_cbvTables[frameIndex]);
	commandList.SetComputeDescriptorTable(1, m_uavSrvTable);
	commandList.SetComputeDescriptorTable(2, m_samplerTable);

	// Dispatch halved cube
	commandList.Dispatch(DIV_UP(m_gridSize.x, 8), DIV_UP(m_gridSize.y, 8), 3);
}

void RayCaster::rayCast(const CommandList& commandList, uint32_t frameIndex)
{
	// Set barriers
	ResourceBarrier barriers[3];
	auto numBarriers = 0u;
	for (auto& slice : m_halvedCube)
		numBarriers = slice.SetBarrier(barriers, ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	commandList.Barrier(numBarriers, barriers);

	// Set pipeline state
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[RAY_CAST]);
	commandList.SetPipelineState(m_pipelines[RAY_CAST]);

	commandList.IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Set descriptor tables
	commandList.SetGraphicsDescriptorTable(0, m_cbvTables[frameIndex]);
	commandList.SetGraphicsDescriptorTable(1, m_srvTables[SRV_TABLE_HALVED_CUBE]);
	commandList.SetGraphicsDescriptorTable(2, m_samplerTable);

	commandList.Draw(3, 1, 0, 0);
}
