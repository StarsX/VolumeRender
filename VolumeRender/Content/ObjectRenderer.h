//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "DXFramework.h"
#include "Core/XUSG.h"

class ObjectRenderer
{
public:
	enum DepthIndex : uint8_t
	{
		DEPTH_MAP,
		SHADOW_MAP,

		NUM_DEPTH
	};

	ObjectRenderer(const XUSG::Device::sptr& device);
	virtual ~ObjectRenderer();

	bool Init(XUSG::CommandList* pCommandList, uint32_t width, uint32_t height,
		const XUSG::DescriptorTableCache::sptr& descriptorTableCache,
		std::vector<XUSG::Resource::uptr>& uploaders, const char* fileName, XUSG::Format rtFormat,
		XUSG::Format dsFormat, const DirectX::XMFLOAT4& posScale = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f));
	bool SetViewport(uint32_t width, uint32_t height, XUSG::Format dsFormat);

	void SetLight(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& color, float intensity);
	void SetAmbient(const DirectX::XMFLOAT3& color, float intensity);
	void UpdateFrame(uint8_t frameIndex, DirectX::CXMMATRIX viewProj, const DirectX::XMFLOAT3& eyePt);
	void RenderShadow(const XUSG::CommandList* pCommandList, uint8_t frameIndex);
	void Render(const XUSG::CommandList* pCommandList, uint8_t frameIndex);

	XUSG::DepthStencil* GetDepthMap(DepthIndex index) const;
	const XUSG::DepthStencil::uptr* GetDepthMaps() const;
	DirectX::FXMMATRIX GetShadowVP() const;

	static const uint8_t FrameCount = 3;

protected:
	enum PipelineIndex : uint8_t
	{
		DEPTH_PASS,
		BASE_PASS,

		NUM_PIPELINE
	};

	bool createVB(XUSG::CommandList* pCommandList, uint32_t numVert,
		uint32_t stride, const uint8_t* pData, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createIB(XUSG::CommandList* pCommandList, uint32_t numIndices,
		const uint32_t* pData, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createInputLayout();
	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat, XUSG::Format dsFormat, XUSG::Format dsFormatH);
	bool createDescriptorTables();

	void renderDepth(const XUSG::CommandList* pCommandList, uint8_t frameIndex, const XUSG::ConstantBuffer* pCb);

	XUSG::Device::sptr m_device;

	XUSG::ShaderPool::uptr				m_shaderPool;
	XUSG::Graphics::PipelineCache::uptr	m_graphicsPipelineCache;
	XUSG::Compute::PipelineCache::uptr	m_computePipelineCache;
	XUSG::PipelineLayoutCache::uptr		m_pipelineLayoutCache;
	XUSG::DescriptorTableCache::sptr	m_descriptorTableCache;

	const XUSG::InputLayout* m_pInputLayout;
	XUSG::PipelineLayout	m_pipelineLayouts[NUM_PIPELINE];
	XUSG::Pipeline			m_pipelines[NUM_PIPELINE];

	XUSG::DescriptorTable	m_cbvTables[FrameCount];
	//XUSG::DescriptorTable	m_samplerTable;

	XUSG::VertexBuffer::uptr	m_vertexBuffer;
	XUSG::IndexBuffer::uptr		m_indexBuffer;

	XUSG::DepthStencil::uptr	m_depths[NUM_DEPTH];
	XUSG::ConstantBuffer::uptr	m_cbShadow;
	XUSG::ConstantBuffer::uptr	m_cbPerObject;
	XUSG::ConstantBuffer::uptr	m_cbPerFrame;

	uint32_t				m_numIndices;
	uint32_t				m_shadowMapSize;
	DirectX::XMUINT2		m_viewport;
	DirectX::XMFLOAT4		m_posScale;

	DirectX::XMFLOAT3		m_lightPt;
	DirectX::XMFLOAT4		m_lightColor;
	DirectX::XMFLOAT4		m_ambient;
	DirectX::XMFLOAT4X4		m_shadowVP;

	float					m_sceneSize;
};
