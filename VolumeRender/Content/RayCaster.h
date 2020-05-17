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

	bool Init(uint32_t width, uint32_t height, XUSG::DescriptorTableCache::sptr descriptorTableCache,
		XUSG::Format rtFormat, XUSG::Format dsFormat, uint32_t gridSize);

	bool LoadVolumeData(XUSG::CommandList* pCommandList, const wchar_t* fileName, std::vector<XUSG::Resource>& uploaders);
	void InitVolumeData(const XUSG::CommandList* pCommandList);
	void SetVolumeWorld(float size, const DirectX::XMFLOAT3& pos);
	void SetLightMapWorld(float size, const DirectX::XMFLOAT3& pos);
	void SetLight(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& color, float intensity);
	void SetAmbient(const DirectX::XMFLOAT3& color, float intensity);
	void UpdateFrame(uint32_t frameIndex, DirectX::CXMMATRIX viewProj, const DirectX::XMFLOAT3& eyePt);
	void Render(const XUSG::CommandList* pCommandList, uint32_t frameIndex, bool splitLightPass = true);
	void RayMarchL(const XUSG::CommandList* pCommandList, uint32_t frameIndex);

	const XUSG::DescriptorTable& GetVolumeSRVTable(const XUSG::CommandList* pCommandList);
	const XUSG::DescriptorTable& GetLightSRVTable() const;
	XUSG::ResourceBase& GetLightMap();

	static const uint32_t FrameCount = 3;

protected:
	enum PipelineIndex : uint8_t
	{
		LOAD_VOLUME_DATA,
		INIT_VOLUME_DATA,
		RAY_MARCH,
		RAY_MARCH_L,
		RAY_MARCH_V,
		RAY_CAST,

		NUM_PIPELINE
	};

	enum SrvTable : uint8_t
	{
		SRV_TABLE_FILE_SRC,
		SRV_TABLE_LIGHT_MAP,
		SRV_TABLE_CUBE_MAP,

		NUM_SRV_TABLE
	};

	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat, XUSG::Format dsFormat);
	bool createDescriptorTables();

	void rayMarch(const XUSG::CommandList* pCommandList, uint32_t frameIndex);
	void rayMarchV(const XUSG::CommandList* pCommandList, uint32_t frameIndex);
	void rayCast(const XUSG::CommandList* pCommandList, uint32_t frameIndex);

	XUSG::Device m_device;

	XUSG::ShaderPool::uptr				m_shaderPool;
	XUSG::Graphics::PipelineCache::uptr	m_graphicsPipelineCache;
	XUSG::Compute::PipelineCache::uptr	m_computePipelineCache;
	XUSG::PipelineLayoutCache::uptr		m_pipelineLayoutCache;
	XUSG::DescriptorTableCache::sptr	m_descriptorTableCache;

	XUSG::PipelineLayout	m_pipelineLayouts[NUM_PIPELINE];
	XUSG::Pipeline			m_pipelines[NUM_PIPELINE];

	XUSG::DescriptorTable	m_cbvTables[FrameCount];
	XUSG::DescriptorTable	m_uavSrvTable;
	XUSG::DescriptorTable	m_srvUavTable;
	XUSG::DescriptorTable	m_srvTables[NUM_SRV_TABLE];
	XUSG::DescriptorTable	m_uavTable;
	XUSG::DescriptorTable	m_samplerTable;

	XUSG::ResourceBase::sptr	m_fileSrc;
	XUSG::Texture3D::uptr		m_volume;
	XUSG::Texture2D::uptr		m_cubeMap;
	XUSG::Texture3D::uptr		m_lightMap;
	XUSG::ConstantBuffer::uptr	m_cbPerObject;

	uint32_t				m_gridSize;
	uint32_t				m_lightGridSize;
	DirectX::XMUINT2		m_viewport;

	DirectX::XMFLOAT3		m_lightPt;
	DirectX::XMFLOAT4		m_lightColor;
	DirectX::XMFLOAT4		m_ambient;
	DirectX::XMFLOAT4X4		m_volumeWorld;
	DirectX::XMFLOAT4X4		m_lightMapWorld;
};
