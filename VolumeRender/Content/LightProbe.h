//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "DXFramework.h"
#include "Core/XUSG.h"

class LightProbe
{
public:
	LightProbe(const XUSG::Device::sptr &device);
	virtual ~LightProbe();

	bool Init(XUSG::CommandList* pCommandList, const XUSG::DescriptorTableCache::sptr& descriptorTableCache,
		std::vector<XUSG::Resource::uptr>& uploaders, const wchar_t* fileName);
	bool CreateDescriptorTables();

	void Process(const XUSG::CommandList* pCommandList, uint8_t frameIndex);

	XUSG::ShaderResource* GetRadiance() const;
	XUSG::StructuredBuffer::sptr GetSH() const;

	static const uint8_t FrameCount = 3;
	static const uint8_t CubeMapFaceCount = 6;

protected:
	enum PipelineIndex : uint8_t
	{
		SH_CUBE_MAP,
		SH_SUM,
		SH_NORMALIZE,

		NUM_PIPELINE
	};

	bool createPipelineLayouts();
	bool createPipelines();
	bool createDescriptorTables();

	void shCubeMap(const XUSG::CommandList* pCommandList, uint8_t order);
	void shSum(const XUSG::CommandList* pCommandList, uint8_t order);
	void shNormalize(const XUSG::CommandList* pCommandList, uint8_t order);
	
	XUSG::Device::sptr m_device;

	XUSG::ShaderPool::uptr				m_shaderPool;
	XUSG::Compute::PipelineCache::uptr	m_computePipelineCache;
	XUSG::PipelineLayoutCache::uptr		m_pipelineLayoutCache;
	XUSG::DescriptorTableCache::sptr	m_descriptorTableCache;

	XUSG::PipelineLayout	m_pipelineLayouts[NUM_PIPELINE];
	XUSG::Pipeline			m_pipelines[NUM_PIPELINE];

	XUSG::DescriptorTable	m_srvTable;
	XUSG::DescriptorTable	m_samplerTable;

	XUSG::ShaderResource::sptr	m_radiance;

	XUSG::StructuredBuffer::sptr m_coeffSH[2];
	XUSG::StructuredBuffer::uptr m_weightSH[2];

	uint32_t				m_numSHTexels;
	uint8_t					m_shBufferParity;
};
