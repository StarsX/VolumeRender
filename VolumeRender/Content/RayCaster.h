//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "DXFramework.h"
#include "Core/XUSG.h"

class RayCaster
{
public:
	enum RenderFlags : uint8_t
	{
		RAY_MARCH_DIRECT	= 0,
		RAY_MARCH_CUBEMAP	= (1 << 0),
		SEPARATE_LIGHT_PASS	= (1 << 1),
		OPTIMIZED = RAY_MARCH_CUBEMAP | SEPARATE_LIGHT_PASS
	};

	RayCaster(const XUSG::Device::sptr& device);
	virtual ~RayCaster();

	bool Init(const XUSG::DescriptorTableCache::sptr& descriptorTableCache, XUSG::Format rtFormat,
		uint32_t gridSize, const XUSG::DepthStencil::uptr* depths);
	bool LoadVolumeData(XUSG::CommandList* pCommandList, const wchar_t* fileName, std::vector<XUSG::Resource::uptr>& uploaders);
	bool SetDepthMaps(const XUSG::DepthStencil::uptr* depths);

	void InitVolumeData(const XUSG::CommandList* pCommandList);
	void SetMaxSamples(uint32_t maxRaySamples, uint32_t maxLightSamples);
	void SetVolumeWorld(float size, const DirectX::XMFLOAT3& pos);
	void SetLightMapWorld(float size, const DirectX::XMFLOAT3& pos);
	void SetLight(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& color, float intensity);
	void SetAmbient(const DirectX::XMFLOAT3& color, float intensity);
	void UpdateFrame(uint8_t frameIndex, DirectX::CXMMATRIX viewProj, DirectX::CXMMATRIX shadowVP, const DirectX::XMFLOAT3& eyePt);
	void Render(const XUSG::CommandList* pCommandList, uint8_t frameIndex, uint8_t flags = OPTIMIZED);
	void RayMarchL(const XUSG::CommandList* pCommandList, uint8_t frameIndex);

	const XUSG::DescriptorTable& GetVolumeSRVTable(const XUSG::CommandList* pCommandList);
	const XUSG::DescriptorTable& GetLightSRVTable() const;
	XUSG::Resource* GetLightMap() const;

	static const uint8_t FrameCount = 3;

protected:
	enum PipelineIndex : uint8_t
	{
		LOAD_VOLUME_DATA,
		INIT_VOLUME_DATA,
		RAY_MARCH,
		RAY_MARCH_L,
		RAY_MARCH_V,
		RAY_CAST,
		RENDER_CUBE,
		DIRECT_RAY_CAST,
		DIRECT_RAY_CAST_V,

		NUM_PIPELINE
	};

	enum SrvTable : uint8_t
	{
		SRV_TABLE_FILE_SRC,
		SRV_TABLE_VOLUME,
		SRV_TABLE_LIGHT_MAP,
		SRV_TABLE_DEPTH,
		SRV_TABLE_SHADOW,

		NUM_SRV_TABLE
	};

	enum DepthIndex : uint8_t
	{
		DEPTH_MAP,
		SHADOW_MAP
	};

	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat);
	bool createDescriptorTables();

	void rayMarch(const XUSG::CommandList* pCommandList, uint8_t frameIndex);
	void rayMarchV(const XUSG::CommandList* pCommandList, uint8_t frameIndex);
	void renderCube(const XUSG::CommandList* pCommandList, uint8_t frameIndex);
	void rayCastCube(const XUSG::CommandList* pCommandList, uint8_t frameIndex);
	void rayCastDirect(const XUSG::CommandList* pCommandList, uint8_t frameIndex);
	void rayCastVDirect(const XUSG::CommandList* pCommandList, uint8_t frameIndex);

	XUSG::Device::sptr m_device;

	XUSG::ShaderPool::uptr				m_shaderPool;
	XUSG::Graphics::PipelineCache::uptr	m_graphicsPipelineCache;
	XUSG::Compute::PipelineCache::uptr	m_computePipelineCache;
	XUSG::PipelineLayoutCache::uptr		m_pipelineLayoutCache;
	XUSG::DescriptorTableCache::sptr	m_descriptorTableCache;

	XUSG::PipelineLayout	m_pipelineLayouts[NUM_PIPELINE];
	XUSG::Pipeline			m_pipelines[NUM_PIPELINE];

	std::vector<XUSG::DescriptorTable> m_uavMipTables;
	std::vector<XUSG::DescriptorTable> m_srvMipTables;
	XUSG::DescriptorTable	m_cbvTables[FrameCount];
	XUSG::DescriptorTable	m_srvUavTable;
	XUSG::DescriptorTable	m_srvTables[NUM_SRV_TABLE];
	XUSG::DescriptorTable	m_uavTable;
	XUSG::DescriptorTable	m_samplerTable;

	XUSG::ShaderResource::sptr	m_fileSrc;
	XUSG::Texture3D::uptr		m_volume;
	XUSG::Texture2D::uptr		m_cubeMap;
	XUSG::Texture2D::uptr		m_cubeDepth;
	XUSG::Texture3D::uptr		m_lightMap;
	XUSG::ConstantBuffer::uptr	m_cbPerObject;
#if _CPU_CUBE_FACE_CULL_ == 2
	XUSG::ConstantBuffer::uptr	m_cbCubeFaceList;
#endif

	const XUSG::DepthStencil::uptr* m_pDepths;

	uint32_t				m_gridSize;
	uint32_t				m_lightGridSize;
	uint32_t				m_raySampleCount;
#if _CPU_CUBE_FACE_CULL_ == 1
	uint32_t				m_visibilityMask;
#endif
	uint32_t				m_maxRaySamples;
	uint32_t				m_maxLightSamples;

	uint8_t					m_cubeFaceCount;
	uint8_t					m_cubeMapLOD;

	DirectX::XMFLOAT3		m_lightPt;
	DirectX::XMFLOAT4		m_lightColor;
	DirectX::XMFLOAT4		m_ambient;
	DirectX::XMFLOAT3X4		m_volumeWorld;
	DirectX::XMFLOAT3X4		m_lightMapWorld;
};
