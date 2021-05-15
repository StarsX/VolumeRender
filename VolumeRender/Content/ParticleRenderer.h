//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "DXFramework.h"
#include "Core/XUSG.h"

class ParticleRenderer
{
public:
	ParticleRenderer(const XUSG::Device& device);
	virtual ~ParticleRenderer();

	bool Init(uint32_t width, uint32_t height, XUSG::DescriptorTableCache::sptr descriptorTableCache,
		XUSG::Format rtFormat, XUSG::Format dsFormat, uint32_t numParticles, float particleSize);

	void GenerateParticles(const XUSG::CommandList* pCommandList, const XUSG::DescriptorTable& srvTable);
	void UpdateFrame(uint8_t frameIndex, DirectX::CXMMATRIX& view, DirectX::CXMMATRIX& proj, const DirectX::XMFLOAT3& eyePt);
	void Render(const XUSG::CommandList* pCommandList, uint8_t frameIndex, XUSG::ResourceBase& lightMap,
		const XUSG::DescriptorTable& srvTable, const XUSG::Descriptor& rtv, const XUSG::Descriptor& dsv);
	void ShowParticles(const XUSG::CommandList* pCommandList, uint8_t frameIndex,
		XUSG::ResourceBase& lightMap, const XUSG::DescriptorTable& srvTable);

	static const uint8_t FrameCount = 3;

protected:
	enum PipelineIndex : uint8_t
	{
		GEN_PARTICLES,
		WEIGHT_BLEND,
		RESOLVE_OIT,
		SHOW_PARTICLES,

		NUM_PIPELINE
	};

	enum SrvTable : uint8_t
	{
		SRV_TABLE_PARTICLES,
		SRV_TABLE_OIT,

		NUM_SRV_TABLE
	};

	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat, XUSG::Format dsFormat);
	bool createDescriptorTables();

	void weightBlend(const XUSG::CommandList* pCommandList, uint8_t frameIndex,
		XUSG::ResourceBase& lightMap, const XUSG::DescriptorTable& srvTable, const XUSG::Descriptor& dsv);
	void resolveOIT(const XUSG::CommandList* pCommandList, const XUSG::Descriptor& rtv);

	XUSG::Device m_device;

	XUSG::ShaderPool::uptr				m_shaderPool;
	XUSG::Graphics::PipelineCache::uptr	m_graphicsPipelineCache;
	XUSG::Compute::PipelineCache::uptr	m_computePipelineCache;
	XUSG::PipelineLayoutCache::uptr		m_pipelineLayoutCache;
	XUSG::DescriptorTableCache::sptr	m_descriptorTableCache;

	XUSG::PipelineLayout	m_pipelineLayouts[NUM_PIPELINE];
	XUSG::Pipeline			m_pipelines[NUM_PIPELINE];

	XUSG::DescriptorTable	m_srvTables[NUM_SRV_TABLE];
	XUSG::DescriptorTable	m_uavTable;
	XUSG::DescriptorTable	m_samplerTable;

	XUSG::StructuredBuffer::uptr	m_particles;
	XUSG::RenderTarget::uptr		m_rtOITs[2];

	XUSG::ConstantBuffer::uptr		m_cbPerObject;

	float					m_particleSize;
	uint32_t				m_numParticles;
	DirectX::XMUINT2		m_viewport;
};
