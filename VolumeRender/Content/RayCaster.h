//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "DXFramework.h"
#include "Core/XUSG.h"

class RayCaster
{
public:
	RayCaster(const XUSG::Device& device);
	virtual ~RayCaster();

	bool Init(const XUSG::CommandList& commandList, uint32_t width, uint32_t height,
		std::shared_ptr<XUSG::DescriptorTableCache> descriptorTableCache,
		std::vector<XUSG::Resource>& uploaders, XUSG::Format rtFormat, XUSG::Format dsFormat,
		const DirectX::XMUINT3& gridSize);

	void InitGridData(const XUSG::CommandList& commandList);
	void UpdateFrame(uint32_t frameIndex, DirectX::CXMMATRIX viewProj, DirectX::CXMVECTOR eyePt);
	void Render(const XUSG::CommandList& commandList, uint32_t frameIndex, bool splitLightPass = true);
	void RayMarchL(const XUSG::CommandList& commandList, uint32_t frameIndex);

	const XUSG::DescriptorTable& GetGridSRVTable(const XUSG::CommandList& commandList);
	const XUSG::DescriptorTable& GetLightSRVTable() const;
	XUSG::ResourceBase& GetLightMap();

	static const uint32_t FrameCount = 3;

protected:
	enum PipelineIndex : uint8_t
	{
		INIT_GRID_DATA,
		RAY_MARCH,
		RAY_MARCH_L,
		RAY_MARCH_V,
		RAY_CAST,

		NUM_PIPELINE
	};

	enum SrvTable : uint8_t
	{
		SRV_TABLE_LIGHT_MAP,
		SRV_TABLE_HALVED_CUBE,

		NUM_SRV_TABLE
	};

	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat, XUSG::Format dsFormat);
	bool createDescriptorTables();

	void rayMarch(const XUSG::CommandList& commandList, uint32_t frameIndex);
	void rayMarchV(const XUSG::CommandList& commandList, uint32_t frameIndex);
	void rayCast(const XUSG::CommandList& commandList, uint32_t frameIndex);

	DirectX::XMMATRIX getWorldMatrix() const;

	XUSG::Device m_device;

	XUSG::ShaderPool				m_shaderPool;
	XUSG::Graphics::PipelineCache	m_graphicsPipelineCache;
	XUSG::Compute::PipelineCache	m_computePipelineCache;
	XUSG::PipelineLayoutCache		m_pipelineLayoutCache;
	std::shared_ptr<XUSG::DescriptorTableCache> m_descriptorTableCache;

	XUSG::PipelineLayout	m_pipelineLayouts[NUM_PIPELINE];
	XUSG::Pipeline			m_pipelines[NUM_PIPELINE];

	XUSG::DescriptorTable	m_cbvTables[FrameCount];
	XUSG::DescriptorTable	m_uavSrvTable;
	XUSG::DescriptorTable	m_srvUavTable;
	XUSG::DescriptorTable	m_srvTables[NUM_SRV_TABLE];
	XUSG::DescriptorTable	m_uavTable;
	XUSG::DescriptorTable	m_samplerTable;

	XUSG::Texture3D			m_grid;
	XUSG::Texture2D			m_halvedCube[3];
	XUSG::Texture3D			m_lightMap;
	XUSG::ConstantBuffer	m_cbPerObject;

	DirectX::XMUINT3		m_gridSize;
	DirectX::XMUINT3		m_lightGridSize;
	DirectX::XMUINT2		m_viewport;
};
