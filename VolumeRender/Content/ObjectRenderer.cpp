//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Optional/XUSGObjLoader.h"
#include "ObjectRenderer.h"
#define _INDEPENDENT_HALTON_
#include "Advanced/XUSGHalton.h"
#undef _INDEPENDENT_HALTON_

using namespace std;
using namespace DirectX;
using namespace XUSG;

enum LightProbeBit : uint8_t
{
	IRRADIANCE_BIT = (1 << 0),
	RADIANCE_BIT = (1 << 1)
};

struct CBPerObject
{
	XMFLOAT4X4 WorldViewProj;
	XMFLOAT4X4 WorldViewProjPrev;
	XMFLOAT3X4 World;
	XMFLOAT4X4 ShadowWVP;
	XMFLOAT2 ProjBias;
};

struct CBPerFrame
{
	XMFLOAT4 EyePos;
	XMFLOAT4 LightPos;
	XMFLOAT4 LightColor;
	XMFLOAT4 Ambient;
};

ObjectRenderer::ObjectRenderer() :
	m_srvTables(),
	m_coeffSH(nullptr),
	m_frameParity(0),
	m_shadowMapSize(1024),
	m_lightPt(75.0f, 75.0f, -75.0f),
	m_lightColor(1.0f, 0.7f, 0.3f, 1.0f),
	m_ambient(0.0f, 0.3f, 1.0f, 0.4f)
{
	m_shaderPool = ShaderPool::MakeUnique();
}

ObjectRenderer::~ObjectRenderer()
{
}

bool ObjectRenderer::Init(CommandList* pCommandList, const DescriptorTableCache::sptr& descriptorTableCache,
	vector<Resource::uptr>& uploaders, const char* fileName, Format backFormat, Format rtFormat,
	Format dsFormat, const XMFLOAT4& posScale)
{
	const auto pDevice = pCommandList->GetDevice();
	m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(pDevice);
	m_computePipelineCache = Compute::PipelineCache::MakeUnique(pDevice);
	m_pipelineLayoutCache = PipelineLayoutCache::MakeUnique(pDevice);
	m_descriptorTableCache = descriptorTableCache;

	SetWorld(posScale.w, XMFLOAT3(posScale.x, posScale.y, posScale.z));

	// Load inputs
	ObjLoader objLoader;
	if (!objLoader.Import(fileName, true, true)) return false;
	XUSG_N_RETURN(createVB(pCommandList, objLoader.GetNumVertices(), objLoader.GetVertexStride(), objLoader.GetVertices(), uploaders), false);
	XUSG_N_RETURN(createIB(pCommandList, objLoader.GetNumIndices(), objLoader.GetIndices(), uploaders), false);
	m_sceneSize = objLoader.GetRadius() * posScale.w * 2.0f;

	// Create resources
	const auto smFormat = Format::D16_UNORM;
	m_depths[SHADOW_MAP] = DepthStencil::MakeUnique();
	XUSG_N_RETURN(m_depths[SHADOW_MAP]->Create(pDevice, m_shadowMapSize, m_shadowMapSize,
		smFormat, ResourceFlag::NONE, 1, 1, 1, 1.0f, 0, false, MemoryFlag::NONE, L"Shadow"), false);

	m_cbShadow = ConstantBuffer::MakeUnique();
	XUSG_N_RETURN(m_cbShadow->Create(pDevice, sizeof(XMFLOAT4X4[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"ObjectRenderer.CBshadow"), false);

	m_cbPerObject = ConstantBuffer::MakeUnique();
	XUSG_N_RETURN(m_cbPerObject->Create(pDevice, sizeof(CBPerObject[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"ObjectRenderer.CBPerObject"), false);

	m_cbPerFrame = ConstantBuffer::MakeUnique();
	XUSG_N_RETURN(m_cbPerFrame->Create(pDevice, sizeof(CBPerFrame[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"ObjectRenderer.CBPerFrame"), false);

	// Create window size-dependent resource
	//XUSG_N_RETURN(SetViewport(width, height, dsFormat), false);

	// Create pipelines
	XUSG_N_RETURN(createInputLayout(), false);
	XUSG_N_RETURN(createPipelineLayouts(), false);
	XUSG_N_RETURN(createPipelines(backFormat, rtFormat, dsFormat, smFormat), false);

	return true;
}

bool ObjectRenderer::SetViewport(const Device* pDevice, uint32_t width, uint32_t height,
	Format rtFormat, Format dsFormat, const float* clearColor, bool needUavRT)
{
	m_viewport = XMUINT2(width, height);

	// Recreate window size-dependent resource
	for (auto& renderTarget : m_renderTargets) renderTarget = RenderTarget::MakeUnique();
	XUSG_N_RETURN(m_renderTargets[RT_COLOR]->Create(pDevice, width, height, rtFormat, 1,
		needUavRT ? ResourceFlag::ALLOW_UNORDERED_ACCESS : ResourceFlag::NONE, 1, 1, clearColor,
		false, MemoryFlag::NONE, L"RenderTarget"), false);
	m_renderTargets[RT_VELOCITY]->Create(pDevice, width, height, Format::R16G16_FLOAT,
		1, ResourceFlag::NONE, 1, 1, nullptr, false, MemoryFlag::NONE, L"Velocity");

	// Temporal AA
	for (uint8_t i = 0; i < 2; ++i)
	{
		auto& temporalView = m_temporalViews[i];
		temporalView = Texture2D::MakeUnique();
		XUSG_N_RETURN(temporalView->Create(pDevice, width, height, Format::R16G16B16A16_FLOAT, 1,
			ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, false, MemoryFlag::NONE,
			(L"TemporalView" + to_wstring(i)).c_str()), false);
	}

	m_depths[DEPTH_MAP] = DepthStencil::MakeUnique();
	XUSG_N_RETURN(m_depths[DEPTH_MAP]->Create(pDevice, width, height, dsFormat,
		ResourceFlag::NONE, 1, 1, 1, 1.0f, 0, false, MemoryFlag::NONE, L"Depth"), false);

	return createDescriptorTables();
}

bool ObjectRenderer::SetRadiance(const Descriptor& radiance)
{
	const auto descriptorTable = Util::DescriptorTable::MakeUnique();
	descriptorTable->SetDescriptors(0, 1, &radiance);
	XUSG_X_RETURN(m_srvTables[SRV_TABLE_RADIANCE], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);

	return true;
}

void ObjectRenderer::SetWorld(float scale, const XMFLOAT3& pos, const XMFLOAT3* pPitchYawRoll)
{
	auto world = XMMatrixScaling(scale, scale, scale);
	if (pPitchYawRoll) world *= XMMatrixRotationRollPitchYaw(pPitchYawRoll->x, pPitchYawRoll->y, pPitchYawRoll->z);
	world = world * XMMatrixTranslation(pos.x, pos.y, pos.z);
	XMStoreFloat3x4(&m_world, world);
}

void ObjectRenderer::SetLight(const XMFLOAT3& pos, const XMFLOAT3& color, float intensity)
{
	m_lightPt = pos;
	m_lightColor = XMFLOAT4(color.x, color.y, color.z, intensity);
}

void ObjectRenderer::SetAmbient(const XMFLOAT3& color, float intensity)
{
	m_ambient = XMFLOAT4(color.x, color.y, color.z, intensity);
}

void ObjectRenderer::SetSH(const StructuredBuffer::sptr& coeffSH)
{
	m_coeffSH = coeffSH;
}

void ObjectRenderer::UpdateFrame(uint8_t frameIndex, CXMMATRIX viewProj, const XMFLOAT3& eyePt)
{
	XMFLOAT4X4 shadowWVP;
	const auto world = XMLoadFloat3x4(&m_world);

	{
		const auto zNear = 1.0f;
		const auto zFar = 200.0f;

		const auto size = m_sceneSize * 1.5f;
		const auto lightPos = XMLoadFloat3(&m_lightPt);
		const auto lightView = XMMatrixLookAtLH(lightPos, XMVectorZero(), XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f));
		const auto lightProj = XMMatrixOrthographicLH(size, size, zNear, zFar);
		const auto lightViewProj = lightView * lightProj;
		XMStoreFloat4x4(&m_shadowVP, XMMatrixTranspose(lightViewProj));
		XMStoreFloat4x4(&shadowWVP, XMMatrixTranspose(world * lightViewProj));

		const auto pCbData = reinterpret_cast<XMFLOAT4X4*>(m_cbShadow->Map(frameIndex));
		*pCbData = shadowWVP;
	}

	const auto halton = IncrementalHalton();
	XMFLOAT2 jitter =
	{
		(halton.x * 2.0f - 1.0f) / m_viewport.x,
		(halton.y * 2.0f - 1.0f) / m_viewport.y
	};

	{
		const auto pCbData = reinterpret_cast<CBPerObject*>(m_cbPerObject->Map(frameIndex));
		XMStoreFloat4x4(&pCbData->WorldViewProj, XMMatrixTranspose(world * viewProj));
		XMStoreFloat3x4(&pCbData->World, world);
		pCbData->ShadowWVP = shadowWVP;
		pCbData->ProjBias = jitter;
		pCbData->WorldViewProjPrev = m_worldViewProj;
		m_worldViewProj = pCbData->WorldViewProj;
	}

	{
		const auto pCbData = reinterpret_cast<CBPerFrame*>(m_cbPerFrame->Map(frameIndex));
		pCbData->EyePos = XMFLOAT4(eyePt.x, eyePt.y, eyePt.z, 1.0f);
		pCbData->LightPos = XMFLOAT4(m_lightPt.x, m_lightPt.y, m_lightPt.z, 1.0f);
		pCbData->LightColor = m_lightColor;
		pCbData->Ambient = m_ambient;
	}

	m_frameParity = !m_frameParity;
}

void ObjectRenderer::RenderShadow(CommandList* pCommandList, uint8_t frameIndex, bool drawScene)
{
	// Set barrier
	ResourceBarrier barrier;
	const auto& shadow = m_depths[SHADOW_MAP];
	const auto numBarriers = shadow->SetBarrier(&barrier, ResourceState::DEPTH_WRITE);
	pCommandList->Barrier(numBarriers, &barrier);

	// Clear depth
	const auto dsv = shadow->GetDSV();
	pCommandList->OMSetRenderTargets(0, nullptr, &dsv);
	pCommandList->ClearDepthStencilView(dsv, ClearFlag::DEPTH, 1.0f);

	if (drawScene)
	{
		// Set shadow viewport
		Viewport viewport(0.0f, 0.0f, static_cast<float>(m_shadowMapSize), static_cast<float>(m_shadowMapSize));
		RectRange scissorRect(0, 0, m_shadowMapSize, m_shadowMapSize);
		pCommandList->RSSetViewports(1, &viewport);
		pCommandList->RSSetScissorRects(1, &scissorRect);

		renderDepth(pCommandList, frameIndex, m_cbShadow.get());
	}
}

void ObjectRenderer::Render(const CommandList* pCommandList, uint8_t frameIndex, bool drawScene)
{
	if (drawScene) render(pCommandList, frameIndex);
}

void ObjectRenderer::Postprocess(CommandList* pCommandList, RenderTarget* pColorOut)
{
	TemporalAA(pCommandList);
	ToneMap(pCommandList, pColorOut);
}

void ObjectRenderer::TemporalAA(CommandList* pCommandList)
{
	ResourceBarrier barriers[4];
	auto numBarriers = m_temporalViews[m_frameParity]->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS);
	numBarriers = m_renderTargets[RT_COLOR]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	numBarriers = m_temporalViews[!m_frameParity]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE |
		ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	numBarriers = m_renderTargets[RT_VELOCITY]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	//numBarriers = m_temporalViews[!m_frameParity]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE |
		//ResourceState::PIXEL_SHADER_RESOURCE, numBarriers, BARRIER_ALL_SUBRESOURCES, BarrierFlag::END_ONLY);
	//numBarriers = m_renderTargets[RT_VELOCITY]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE,
		//numBarriers, BARRIER_ALL_SUBRESOURCES, BarrierFlag::END_ONLY);
	pCommandList->Barrier(numBarriers, barriers);

	// Set descriptor tables
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[TEMPORAL_AA]);
	pCommandList->SetComputeDescriptorTable(0, m_uavTables[UAV_TABLE_TAA + m_frameParity]);
	pCommandList->SetComputeDescriptorTable(1, m_srvTables[SRV_TABLE_TAA + m_frameParity]);

	// Set pipeline state
	pCommandList->SetPipelineState(m_pipelines[TEMPORAL_AA]);
	pCommandList->Dispatch(XUSG_DIV_UP(m_viewport.x, 8), XUSG_DIV_UP(m_viewport.y, 8), 1);
}

void ObjectRenderer::ToneMap(CommandList* pCommandList, RenderTarget* pColorOut)
{
	ResourceBarrier barriers[2];
	auto numBarriers = pColorOut->SetBarrier(barriers, ResourceState::RENDER_TARGET);
	numBarriers = m_temporalViews[m_frameParity]->SetBarrier(
		barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE |
		ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);

	// Set render target
	pCommandList->OMSetRenderTargets(1, &pColorOut->GetRTV());

	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[TONE_MAP]);
	pCommandList->SetPipelineState(m_pipelines[TONE_MAP]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Set descriptor tables
	pCommandList->SetGraphicsDescriptorTable(0, m_srvTables[SRV_TABLE_PP + m_frameParity]);
	//pCommandList->SetGraphicsDescriptorTable(0, m_srvTables[SRV_TABLE_TAA]);

	pCommandList->Draw(3, 1, 0, 0);
}

RenderTarget* ObjectRenderer::GetRenderTarget(RenderTargetIndex index) const
{
	return m_renderTargets[index].get();
}

DepthStencil* ObjectRenderer::GetDepthMap(DepthIndex index) const
{
	return m_depths[index].get();
}

const DepthStencil::uptr* ObjectRenderer::GetDepthMaps() const
{
	return m_depths;
}

const XMFLOAT4X4& ObjectRenderer::GetShadowVP() const
{
	return m_shadowVP;
}

bool ObjectRenderer::createVB(CommandList* pCommandList, uint32_t numVert,
	uint32_t stride, const uint8_t* pData, vector<Resource::uptr>& uploaders)
{
	m_vertexBuffer = VertexBuffer::MakeUnique();
	XUSG_N_RETURN(m_vertexBuffer->Create(pCommandList->GetDevice(), numVert, stride, ResourceFlag::NONE,
		MemoryType::DEFAULT, 1, nullptr, 1, nullptr, 1, nullptr, MemoryFlag::NONE, L"MeshVB"), false);
	uploaders.emplace_back(Resource::MakeUnique());

	return m_vertexBuffer->Upload(pCommandList, uploaders.back().get(), pData, stride * numVert);
}

bool ObjectRenderer::createIB(CommandList* pCommandList, uint32_t numIndices,
	const uint32_t* pData, vector<Resource::uptr>& uploaders)
{
	m_numIndices = numIndices;

	const uint32_t byteWidth = sizeof(uint32_t) * numIndices;
	m_indexBuffer = IndexBuffer::MakeUnique();
	XUSG_N_RETURN(m_indexBuffer->Create(pCommandList->GetDevice(), byteWidth, Format::R32_UINT, ResourceFlag::NONE,
		MemoryType::DEFAULT, 1, nullptr, 1, nullptr, 1, nullptr, MemoryFlag::NONE, L"MeshIB"), false);
	uploaders.emplace_back(Resource::MakeUnique());

	return m_indexBuffer->Upload(pCommandList, uploaders.back().get(), pData, byteWidth);
}

bool ObjectRenderer::createInputLayout()
{
	// Define the vertex input layout.
	const InputElement inputElements[] =
	{
		{ "POSITION",	0, Format::R32G32B32_FLOAT, 0, 0,								InputClassification::PER_VERTEX_DATA, 0 },
		{ "NORMAL",		0, Format::R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,	InputClassification::PER_VERTEX_DATA, 0 }
	};

	XUSG_X_RETURN(m_pInputLayout, m_graphicsPipelineCache->CreateInputLayout(inputElements, static_cast<uint32_t>(size(inputElements))), false);

	return true;
}

bool ObjectRenderer::createPipelineLayouts()
{
	// Depth pass
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRootCBV(0, 0, 0, Shader::Stage::VS);
		XUSG_X_RETURN(m_pipelineLayouts[DEPTH_PASS], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"DepthPassLayout"), false);
	}

	// Base pass
	{
		const Sampler samplers[] =
		{
			m_descriptorTableCache->GetSampler(SamplerPreset::LINEAR_LESS_EQUAL),
			m_descriptorTableCache->GetSampler(SamplerPreset::LINEAR_WRAP)
		};

		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRootCBV(0, 0, 0, Shader::Stage::VS);
		pipelineLayout->SetRootCBV(1, 0, 0, Shader::Stage::PS);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetConstants(3, 1, 1, 0, Shader::Stage::PS);
		pipelineLayout->SetRootSRV(4, 1, 0, DescriptorFlag::NONE, Shader::Stage::PS);
		pipelineLayout->SetRange(5, DescriptorType::SRV, 1, 2);
		pipelineLayout->SetStaticSamplers(samplers, static_cast<uint32_t>(size(samplers)), 0, 0, Shader::PS);
		pipelineLayout->SetShaderStage(0, Shader::Stage::VS);
		pipelineLayout->SetShaderStage(1, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(2, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(5, Shader::Stage::PS);
		XUSG_X_RETURN(m_pipelineLayouts[BASE_PASS], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"BasePassLayout"), false);
	}

	// Temporal AA
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::UAV, 1, 0, 0,
			DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 3, 0);
		pipelineLayout->SetStaticSamplers(&m_descriptorTableCache->GetSampler(SamplerPreset::LINEAR_WRAP), 1, 0);
		XUSG_X_RETURN(m_pipelineLayouts[TEMPORAL_AA], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"TemporalAALayout"), false);
	}

	// Tone mapping
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
		XUSG_X_RETURN(m_pipelineLayouts[TONE_MAP], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"ToneMappingLayout"), false);
	}

	return true;
}

bool ObjectRenderer::createPipelines(Format backFormat, Format rtFormat, Format dsFormat, Format dsFormatH)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	// Depth pass
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSDepth.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[DEPTH_PASS]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->IASetInputLayout(m_pInputLayout);
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		//state->DSSetState(Graphics::DepthStencilPreset::DEFAULT_LESS, m_graphicsPipelineCache.get());
		state->OMSetDSVFormat(dsFormatH);
		XUSG_X_RETURN(m_pipelines[DEPTH_PASS], state->GetPipeline(m_graphicsPipelineCache.get(), L"BasePass"), false);
	}

	// Base pass
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSBasePass.cso"), false);
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSBasePass.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[BASE_PASS]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetInputLayout(m_pInputLayout);
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		//state->DSSetState(Graphics::DepthStencilPreset::DEFAULT_LESS, m_graphicsPipelineCache.get());
		state->OMSetNumRenderTargets(2);
		state->OMSetRTVFormat(0, rtFormat);
		state->OMSetRTVFormat(1, Format::R16G16_FLOAT);
		state->OMSetDSVFormat(dsFormat);
		XUSG_X_RETURN(m_pipelines[BASE_PASS], state->GetPipeline(m_graphicsPipelineCache.get(), L"BasePass"), false);
	}

	// Temporal AA
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSTemporalAA.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[TEMPORAL_AA]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[TEMPORAL_AA], state->GetPipeline(m_computePipelineCache.get(), L"TemporalAA"), false);
	}

	// Tone mapping
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSToneMap.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[TONE_MAP]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->DSSetState(Graphics::DepthStencilPreset::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->OMSetRTVFormats(&backFormat, 1);
		XUSG_X_RETURN(m_pipelines[TONE_MAP], state->GetPipeline(m_graphicsPipelineCache.get(), L"ToneMapping"), false);
	}

	return true;
}

bool ObjectRenderer::createDescriptorTables()
{
	// Temporal AA output UAVs
	for (auto i = 0u; i < 2; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_temporalViews[i]->GetUAV());
		XUSG_X_RETURN(m_uavTables[UAV_TABLE_TAA + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Temporal AA input SRVs
	for (auto i = 0u; i < 2; ++i)
	{
		const Descriptor descriptors[] =
		{
			m_renderTargets[RT_COLOR]->GetSRV(),
			m_temporalViews[!i]->GetSRV(),
			m_renderTargets[RT_VELOCITY]->GetSRV()
		};
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_TAA + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Postprocess SRVs
	for (auto i = 0u; i < 2; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_temporalViews[i]->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_PP + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_depths[DEPTH_MAP]->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_DEPTH], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_depths[SHADOW_MAP]->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_SHADOW], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	return true;
}

void ObjectRenderer::render(const CommandList* pCommandList, uint8_t frameIndex)
{
	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[BASE_PASS]);
	pCommandList->SetPipelineState(m_pipelines[BASE_PASS]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Set descriptor tables
	uint8_t hasLightProbes = m_coeffSH ? IRRADIANCE_BIT : 0;
	hasLightProbes |= m_srvTables[SRV_TABLE_RADIANCE] ? RADIANCE_BIT : 0;
	pCommandList->SetGraphicsRootConstantBufferView(0, m_cbPerObject.get(), m_cbPerObject->GetCBVOffset(frameIndex));
	pCommandList->SetGraphicsRootConstantBufferView(1, m_cbPerFrame.get(), m_cbPerFrame->GetCBVOffset(frameIndex));
	pCommandList->SetGraphicsDescriptorTable(2, m_srvTables[SRV_TABLE_SHADOW]);
	pCommandList->SetGraphics32BitConstant(3, hasLightProbes);
	if (m_coeffSH) pCommandList->SetGraphicsRootShaderResourceView(4, m_coeffSH.get());
	if (m_srvTables[SRV_TABLE_RADIANCE]) pCommandList->SetGraphicsDescriptorTable(5, m_srvTables[SRV_TABLE_RADIANCE]);
	pCommandList->IASetVertexBuffers(0, 1, &m_vertexBuffer->GetVBV());
	pCommandList->IASetIndexBuffer(m_indexBuffer->GetIBV());

	pCommandList->DrawIndexed(m_numIndices, 1, 0, 0, 0);
}

void ObjectRenderer::renderDepth(const CommandList* pCommandList, uint8_t frameIndex, const ConstantBuffer* pCb)
{
	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[DEPTH_PASS]);
	pCommandList->SetPipelineState(m_pipelines[DEPTH_PASS]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Set descriptor table
	pCommandList->SetGraphicsRootConstantBufferView(0, pCb, pCb->GetCBVOffset(frameIndex));

	pCommandList->IASetVertexBuffers(0, 1, &m_vertexBuffer->GetVBV());
	pCommandList->IASetIndexBuffer(m_indexBuffer->GetIBV());

	pCommandList->DrawIndexed(m_numIndices, 1, 0, 0, 0);
}
