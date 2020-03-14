//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "DXFramework.h"
#include "Core/XUSG.h"

class ParticleSys
{
public:
	ParticleSys(const XUSG::Device& device);
	virtual ~ParticleSys();

	bool Init(const XUSG::CommandList& commandList, uint32_t width, uint32_t height,
		std::shared_ptr<XUSG::DescriptorTableCache> descriptorTableCache,
		std::vector<XUSG::Resource>& uploaders, XUSG::Format rtFormat,
		XUSG::Format dsFormat, uint32_t numParticles);

	void GenerateParticles(const XUSG::CommandList& commandList, const XUSG::DescriptorTable& srvTable);
	void UpdateFrame(DirectX::CXMMATRIX& view, DirectX::CXMMATRIX& proj, const DirectX::XMFLOAT3& eyePt);
	void Render(const XUSG::CommandList& commandList, XUSG::ResourceBase& lightMap,
		const XUSG::DescriptorTable& srvTable, const XUSG::Descriptor& rtv,
		const XUSG::Descriptor& dsv);
	void ShowParticles(const XUSG::CommandList& commandList, XUSG::ResourceBase& lightMap,
		const XUSG::DescriptorTable& srvTable);

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

	struct CBPerObject
	{
		DirectX::XMFLOAT4X4 WorldView;
		DirectX::XMFLOAT4X4 Proj;
		DirectX::XMFLOAT3 EyePt;
	};

	struct ParticleInfo
	{
		DirectX::XMFLOAT3 Pos;
		DirectX::XMFLOAT3 Velocity;
		float LifeTime;
	};

	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat, XUSG::Format dsFormat);
	bool createDescriptorTables();

	void weightBlend(const XUSG::CommandList& commandList, XUSG::ResourceBase& lightMap,
		const XUSG::DescriptorTable& srvTable, const XUSG::Descriptor& dsv);
	void resolveOIT(const XUSG::CommandList& commandList, const XUSG::Descriptor& rtv);

	DirectX::XMMATRIX getWorldMatrix() const;

	XUSG::Device m_device;

	XUSG::ShaderPool				m_shaderPool;
	XUSG::Graphics::PipelineCache	m_graphicsPipelineCache;
	XUSG::Compute::PipelineCache	m_computePipelineCache;
	XUSG::PipelineLayoutCache		m_pipelineLayoutCache;
	std::shared_ptr<XUSG::DescriptorTableCache> m_descriptorTableCache;

	XUSG::PipelineLayout	m_pipelineLayouts[NUM_PIPELINE];
	XUSG::Pipeline			m_pipelines[NUM_PIPELINE];

	XUSG::DescriptorTable	m_srvTables[NUM_SRV_TABLE];
	XUSG::DescriptorTable	m_uavTable;
	XUSG::DescriptorTable	m_samplerTable;

	XUSG::StructuredBuffer	m_particles;
	XUSG::RenderTarget		m_rtOITs[2];

	uint32_t				m_numParticles;
	DirectX::XMUINT2		m_viewport;
	CBPerObject				m_cbPerObject;
};
