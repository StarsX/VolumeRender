//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "stdafx.h"
#include "ObjectRenderer.h"

using namespace std;
using namespace DirectX;
using namespace XUSG;

struct CBPerObject
{
	XMMATRIX WorldViewProj;
	XMFLOAT3X4 World;
};

ObjectRenderer::ObjectRenderer(const Device::sptr& device) :
	m_device(device),
	m_shadowMapSize(1024),
	m_lightPt(75.0f, 75.0f, -75.0f),
	m_lightColor(1.0f, 0.7f, 0.3f, 1.0f),
	m_ambient(0.0f, 0.3f, 1.0f, 0.3f)
{
	m_shaderPool = ShaderPool::MakeUnique();
	m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(device.get());
	m_computePipelineCache = Compute::PipelineCache::MakeUnique(device.get());
	m_pipelineLayoutCache = PipelineLayoutCache::MakeUnique(device.get());
}

ObjectRenderer::~ObjectRenderer()
{
}

bool ObjectRenderer::Init(uint32_t width, uint32_t height,
	const DescriptorTableCache::sptr& descriptorTableCache,
	Format rtFormat, Format dsFormat)
{
	m_viewport = XMUINT2(width, height);
	m_descriptorTableCache = descriptorTableCache;

	// Create resources
	for (auto& depth : m_depths) depth = DepthStencil::MakeUnique();
	N_RETURN(m_depths[DEPTH_MAP]->Create(m_device.get(), width, height, Format::D32_FLOAT,
		ResourceFlag::NONE, 1, 1, 1, 1.0f, 0, false, L"Depth"), false);
	N_RETURN(m_depths[SHADOW_MAP]->Create(m_device.get(), m_shadowMapSize, m_shadowMapSize, Format::D16_UNORM,
		ResourceFlag::NONE, 1, 1, 1, 1.0f, 0, false, L"Shadow"), false);

	m_cbPerObject = ConstantBuffer::MakeUnique();
	N_RETURN(m_cbPerObject->Create(m_device.get(), sizeof(CBPerObject[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, L"ObjectRenderer.CBPerObject"), false);

	// Create pipelines
	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat, dsFormat), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

bool ObjectRenderer::createPipelineLayouts()
{
	// Base pass
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetShaderStage(0, Shader::Stage::VS);
		X_RETURN(m_pipelineLayouts[BASE_PASS], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"BasePassLayout"), false);
	}

	return true;
}

bool ObjectRenderer::createPipelines(XUSG::Format rtFormat, XUSG::Format dsFormat)
{
	return true;
}

bool ObjectRenderer::createDescriptorTables()
{
	return true;
}
