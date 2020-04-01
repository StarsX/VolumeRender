//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "ParticleRenderer.h"

using namespace std;
using namespace DirectX;
using namespace XUSG;

ParticleRenderer::ParticleRenderer(const Device& device) :
	m_device(device)
{
	m_graphicsPipelineCache.SetDevice(device);
	m_computePipelineCache.SetDevice(device);
	m_pipelineLayoutCache.SetDevice(device);
}

ParticleRenderer::~ParticleRenderer()
{
}

bool ParticleRenderer::Init(const CommandList& commandList, uint32_t width, uint32_t height,
	shared_ptr<DescriptorTableCache> descriptorTableCache, vector<Resource>& uploaders,
	Format rtFormat, Format dsFormat, uint32_t numParticles)
{
	m_viewport = XMUINT2(width, height);
	m_descriptorTableCache = descriptorTableCache;
	m_numParticles = numParticles;

	// Create resources
	N_RETURN(m_particles.Create(m_device, numParticles, sizeof(float[8]), ResourceFlag::ALLOW_UNORDERED_ACCESS,
		MemoryType::DEFAULT, 1, nullptr, 1, nullptr, L"Particles"), false);

	const float clearTransm[4] = { 1.0f };
	N_RETURN(m_rtOITs[0].Create(m_device, width, height, Format::R16G16B16A16_FLOAT, 1,
		ResourceFlag::NONE, 1, 1, nullptr, false, L"OITColor"), false);
	N_RETURN(m_rtOITs[1].Create(m_device, width, height, Format::R8_UNORM, 1,
		ResourceFlag::NONE, 1, 1, clearTransm, false, L"OITTransmittance"), false);

	// Create pipelines
	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat, dsFormat), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

void ParticleRenderer::GenerateParticles(const CommandList& commandList, const DescriptorTable& srvTable)
{
	// Record commands.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	// Set pipeline state
	commandList.SetComputePipelineLayout(m_pipelineLayouts[GEN_PARTICLES]);
	commandList.SetPipelineState(m_pipelines[GEN_PARTICLES]);

	// Set descriptor tables
	commandList.SetComputeDescriptorTable(0, srvTable);
	commandList.SetComputeDescriptorTable(1, m_uavTable);
	commandList.SetComputeDescriptorTable(2, m_samplerTable);

	// Dispatch particles
	commandList.Dispatch(DIV_UP(m_numParticles, 64), 1, 1);
}

void ParticleRenderer::UpdateFrame(CXMMATRIX& view, CXMMATRIX& proj, const XMFLOAT3& eyePt)
{
	// General matrices
	const auto world = getWorldMatrix();
	const auto worldView = world * view;
	const auto worldViewI = XMMatrixInverse(nullptr, worldView);
	XMStoreFloat4x4(&m_cbPerObject.WorldView, XMMatrixTranspose(worldView));
	XMStoreFloat4x4(&m_cbPerObject.WorldViewI, XMMatrixTranspose(worldViewI));
	XMStoreFloat4x4(&m_cbPerObject.Proj, XMMatrixTranspose(proj));
	m_cbPerObject.EyePt = eyePt;

	const auto numParticlePerDim = powf(static_cast<float>(m_numParticles), 1.0f / 3.0f);
	m_cbPerObject.ParticleRadius = XM_PI / numParticlePerDim * XMVectorGetX(world.r[0]);
}

void ParticleRenderer::Render(const CommandList& commandList, ResourceBase& lightMap,
	const DescriptorTable& srvTable, const Descriptor& rtv, const Descriptor& dsv)
{
	weightBlend(commandList, lightMap, srvTable, dsv);
	resolveOIT(commandList, rtv);
}

void ParticleRenderer::ShowParticles(const CommandList& commandList, ResourceBase& lightMap,
	const DescriptorTable& srvTable)
{
	// Set barriers
	ResourceBarrier barriers[2];
	auto numBarriers = m_particles.SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE);
	numBarriers = lightMap.SetBarrier(barriers, ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	commandList.Barrier(numBarriers, barriers);

	// Set pipeline state
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[SHOW_PARTICLES]);
	commandList.SetPipelineState(m_pipelines[SHOW_PARTICLES]);

	commandList.IASetPrimitiveTopology(PrimitiveTopology::TRIANGLESTRIP);

	// Set descriptor tables
	commandList.SetGraphics32BitConstants(0, SizeOfInUint32(m_cbPerObject), &m_cbPerObject);
	commandList.SetGraphicsDescriptorTable(1, m_srvTables[SRV_TABLE_PARTICLES]);
	commandList.SetGraphicsDescriptorTable(2, srvTable);
	commandList.SetGraphicsDescriptorTable(3, m_samplerTable);

	commandList.Draw(4, m_numParticles, 0, 0);
}

bool ParticleRenderer::createPipelineLayouts()
{
	// Generate particles
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout.SetRange(2, DescriptorType::SAMPLER, 1, 0);
		X_RETURN(m_pipelineLayouts[GEN_PARTICLES], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"ParticleGenLayout"), false);
	}

	// Weight blending
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetConstants(0, SizeOfInUint32(CBPerObject), 0, 0, Shader::VS);
		pipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		pipelineLayout.SetRange(2, DescriptorType::SRV, 1, 0);
		pipelineLayout.SetRange(3, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout.SetShaderStage(1, Shader::VS);
		pipelineLayout.SetShaderStage(2, Shader::PS);
		pipelineLayout.SetShaderStage(3, Shader::PS);
		X_RETURN(m_pipelineLayouts[WEIGHT_BLEND], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L" WeightBlendingLayout"), false);
	}

	// OIT resolving
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(0, DescriptorType::SRV, 2, 0);
		pipelineLayout.SetRange(1, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout.SetShaderStage(0, Shader::Stage::PS);
		pipelineLayout.SetShaderStage(1, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[RESOLVE_OIT], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"OITResolvingLayout"), false);
	}

	// Show particles
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetConstants(0, SizeOfInUint32(CBPerObject), 0, 0, Shader::VS);
		pipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		pipelineLayout.SetRange(2, DescriptorType::SRV, 1, 0);
		pipelineLayout.SetRange(3, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout.SetShaderStage(1, Shader::VS);
		pipelineLayout.SetShaderStage(2, Shader::PS);
		pipelineLayout.SetShaderStage(3, Shader::PS);
		X_RETURN(m_pipelineLayouts[SHOW_PARTICLES], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"ParticleShowLayout"), false);
	}

	return true;
}

bool ParticleRenderer::createPipelines(Format rtFormat, Format dsFormat)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	// Generate particles
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, csIndex, L"CSGenParticle.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[GEN_PARTICLES]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[GEN_PARTICLES], state.GetPipeline(m_computePipelineCache, L"ParticleGen"), false);

	}

	// Weight blending
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::VS, vsIndex, L"VSParticle.cso"), false);
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::PS, psIndex, L"PSWeightBlend.cso"), false);

		Graphics::State state;
		state.SetPipelineLayout(m_pipelineLayouts[WEIGHT_BLEND]);
		state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, vsIndex++));
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, psIndex++));
		state.IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state.DSSetState(Graphics::DEPTH_READ_LESS, m_graphicsPipelineCache);
		state.OMSetBlendState(Graphics::WEIGHTED_PER_RT, m_graphicsPipelineCache);
		state.OMSetNumRenderTargets(2);
		state.OMSetRTVFormat(0, Format::R16G16B16A16_FLOAT);
		state.OMSetRTVFormat(1, Format::R8_UNORM);
		state.OMSetDSVFormat(dsFormat);
		X_RETURN(m_pipelines[WEIGHT_BLEND], state.GetPipeline(m_graphicsPipelineCache, L"WeightBlending"), false);
	}

	// OIT resolving
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::PS, psIndex, L"PSResolveOIT.cso"), false);

		Graphics::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RESOLVE_OIT]);
		state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, vsIndex++));
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, psIndex++));
		state.IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state.DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache);
		state.OMSetBlendState(Graphics::NON_PRE_MUL, m_graphicsPipelineCache);
		state.OMSetRTVFormats(&rtFormat, 1);
		X_RETURN(m_pipelines[RESOLVE_OIT], state.GetPipeline(m_graphicsPipelineCache, L"OITResolving"), false);
	}

	// Show particles
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::VS, vsIndex, L"VSParticle.cso"), false);
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::PS, psIndex, L"PSParticle.cso"), false);

		Graphics::State state;
		state.SetPipelineLayout(m_pipelineLayouts[SHOW_PARTICLES]);
		state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, vsIndex++));
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, psIndex++));
		state.IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state.DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache);
		state.OMSetBlendState(Graphics::NON_PRE_MUL, m_graphicsPipelineCache);
		state.OMSetRTVFormats(&rtFormat, 1);
		X_RETURN(m_pipelines[SHOW_PARTICLES], state.GetPipeline(m_graphicsPipelineCache, L"ParticleShow"), false);
	}

	return true;
}

bool ParticleRenderer::createDescriptorTables()
{
	// Create UAV table
	{
		Util::DescriptorTable uavTable;
		uavTable.SetDescriptors(0, 1, &m_particles.GetUAV());
		X_RETURN(m_uavTable, uavTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create SRV tables
	{
		Util::DescriptorTable srvTable;
		const Descriptor descriptors[] =
		{
			m_particles.GetSRV()
		};
		srvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_PARTICLES], srvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	{
		Util::DescriptorTable srvTable;
		const Descriptor descriptors[] =
		{
			m_rtOITs[0].GetSRV(),
			m_rtOITs[1].GetSRV()
		};
		srvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_OIT], srvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create the sampler
	Util::DescriptorTable samplerTable;
	const auto samplerLinearClamp = SamplerPreset::LINEAR_CLAMP;
	samplerTable.SetSamplers(0, 1, &samplerLinearClamp, *m_descriptorTableCache);
	X_RETURN(m_samplerTable, samplerTable.GetSamplerTable(*m_descriptorTableCache), false);

	return true;
}

void ParticleRenderer::weightBlend(const CommandList& commandList, ResourceBase& lightMap,
	const DescriptorTable& srvTable, const Descriptor& dsv)
{
	// Set barriers
	ResourceBarrier barriers[4];
	auto numBarriers = m_rtOITs[0].SetBarrier(barriers, ResourceState::RENDER_TARGET);
	numBarriers = m_rtOITs[1].SetBarrier(barriers, ResourceState::RENDER_TARGET, numBarriers);
	numBarriers = m_particles.SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	numBarriers = lightMap.SetBarrier(barriers, ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	commandList.Barrier(numBarriers, barriers);

	// Set render targets
	const Descriptor rtvs[] = { m_rtOITs[0].GetRTV(), m_rtOITs[1].GetRTV() };
	commandList.OMSetRenderTargets(2, rtvs, &dsv);

	// Clear
	const float clearColor[4] = { };
	const float clearTransm[4] = { 1.0f };
	commandList.ClearRenderTargetView(m_rtOITs[0].GetRTV(), clearColor);
	commandList.ClearRenderTargetView(m_rtOITs[1].GetRTV(), clearTransm);

	// Set pipeline state
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[WEIGHT_BLEND]);
	commandList.SetPipelineState(m_pipelines[WEIGHT_BLEND]);

	commandList.IASetPrimitiveTopology(PrimitiveTopology::TRIANGLESTRIP);

	// Set descriptor tables
	commandList.SetGraphics32BitConstants(0, SizeOfInUint32(m_cbPerObject), &m_cbPerObject);
	commandList.SetGraphicsDescriptorTable(1, m_srvTables[SRV_TABLE_PARTICLES]);
	commandList.SetGraphicsDescriptorTable(2, srvTable);
	commandList.SetGraphicsDescriptorTable(3, m_samplerTable);

	commandList.Draw(4, m_numParticles, 0, 0);
}

void ParticleRenderer::resolveOIT(const CommandList& commandList, const Descriptor& rtv)
{
	// Set barriers
	ResourceBarrier barriers[2];
	auto numBarriers = m_rtOITs[0].SetBarrier(barriers, ResourceState::PIXEL_SHADER_RESOURCE);
	numBarriers = m_rtOITs[1].SetBarrier(barriers, ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	commandList.Barrier(numBarriers, barriers);

	// Set render target
	commandList.OMSetRenderTargets(1, &rtv);

	// Set pipeline state
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[RESOLVE_OIT]);
	commandList.SetPipelineState(m_pipelines[RESOLVE_OIT]);

	commandList.IASetPrimitiveTopology(PrimitiveTopology::TRIANGLESTRIP);

	// Set descriptor tables
	commandList.SetGraphicsDescriptorTable(0, m_srvTables[SRV_TABLE_OIT]);
	commandList.SetGraphicsDescriptorTable(1, m_samplerTable);

	commandList.Draw(3, 1, 0, 0);
}

XMMATRIX ParticleRenderer::getWorldMatrix() const
{
	return XMMatrixScaling(10.0f, 10.0f, 10.0f);
}
