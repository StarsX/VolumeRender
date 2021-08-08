//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Optional/XUSGObjLoader.h"
#include "ObjectRenderer.h"
#define _INDEPENDENT_DDS_LOADER_
#include "Advanced/XUSGDDSLoader.h"
#undef _INDEPENDENT_DDS_LOADER_

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
	XMFLOAT3X4 World;
	XMFLOAT4X4 ShadowWVP;
};

struct CBPerFrame
{
	XMFLOAT4 EyePos;
	XMFLOAT4 LightPos;
	XMFLOAT4 LightColor;
	XMFLOAT4 Ambient;
};

struct CBPerFrameEnv
{
	DirectX::XMFLOAT4	EyePos;
	DirectX::XMFLOAT4X4	ScreenToWorld;
};

ObjectRenderer::ObjectRenderer(const Device::sptr& device) :
	m_device(device),
	m_lightProbes(),
	m_shadowMapSize(1024),
	m_lightPt(75.0f, 75.0f, -75.0f),
	m_lightColor(1.0f, 0.7f, 0.3f, 1.0f),
	m_ambient(0.0f, 0.3f, 1.0f, 0.4f)
{
	m_shaderPool = ShaderPool::MakeUnique();
	m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(device.get());
	m_computePipelineCache = Compute::PipelineCache::MakeUnique(device.get());
	m_pipelineLayoutCache = PipelineLayoutCache::MakeUnique(device.get());
}

ObjectRenderer::~ObjectRenderer()
{
}

bool ObjectRenderer::Init(CommandList* pCommandList, uint32_t width, uint32_t height,
	const DescriptorTableCache::sptr& descriptorTableCache, vector<Resource::uptr>& uploaders,
	const char* fileName, const wchar_t* irradianceMapFileName, const wchar_t* radianceMapFileName,
	Format backFormat, Format rtFormat, Format dsFormat, const XMFLOAT4& posScale)
{
	m_descriptorTableCache = descriptorTableCache;
	m_posScale = posScale;

	// Load inputs
	ObjLoader objLoader;
	if (!objLoader.Import(fileName, true, true)) return false;
	N_RETURN(createVB(pCommandList, objLoader.GetNumVertices(), objLoader.GetVertexStride(), objLoader.GetVertices(), uploaders), false);
	N_RETURN(createIB(pCommandList, objLoader.GetNumIndices(), objLoader.GetIndices(), uploaders), false);
	m_sceneSize = objLoader.GetRadius() * posScale.w * 2.0f;

	// Load input images
	if (irradianceMapFileName && *irradianceMapFileName)
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		uploaders.emplace_back(Resource::MakeUnique());
		N_RETURN(textureLoader.CreateTextureFromFile(m_device.get(), pCommandList, irradianceMapFileName,
			8192, false, m_lightProbes[IRRADIANCE_MAP], uploaders.back().get(), &alphaMode), false);
	}

	if (radianceMapFileName && *radianceMapFileName)
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		uploaders.emplace_back(Resource::MakeUnique());
		N_RETURN(textureLoader.CreateTextureFromFile(m_device.get(), pCommandList, radianceMapFileName,
			8192, false, m_lightProbes[RADIANCE_MAP], uploaders.back().get(), &alphaMode), false);
	}

	// Create resources
	const auto smFormat = Format::D16_UNORM;
	m_depths[SHADOW_MAP] = DepthStencil::MakeUnique();
	N_RETURN(m_depths[SHADOW_MAP]->Create(m_device.get(), m_shadowMapSize, m_shadowMapSize,
		smFormat, ResourceFlag::NONE, 1, 1, 1, 1.0f, 0, false, L"Shadow"), false);

	m_cbShadow = ConstantBuffer::MakeUnique();
	N_RETURN(m_cbShadow->Create(m_device.get(), sizeof(XMFLOAT4X4[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, L"ObjectRenderer.CBshadow"), false);

	m_cbPerObject = ConstantBuffer::MakeUnique();
	N_RETURN(m_cbPerObject->Create(m_device.get(), sizeof(CBPerObject[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, L"ObjectRenderer.CBPerObject"), false);

	m_cbPerFrame = ConstantBuffer::MakeUnique();
	N_RETURN(m_cbPerFrame->Create(m_device.get(), sizeof(CBPerFrame[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, L"ObjectRenderer.CBPerFrame"), false);

	if (m_lightProbes[RADIANCE_MAP])
	{
		m_cbPerFrameEnv = ConstantBuffer::MakeUnique();
		N_RETURN(m_cbPerFrameEnv->Create(m_device.get(), sizeof(CBPerFrameEnv[FrameCount]), FrameCount,
			nullptr, MemoryType::UPLOAD, L"ObjectRenderer.CBPerFrameEnv"), false);
	}

	// Create window size-dependent resource
	//N_RETURN(SetViewport(width, height, dsFormat), false);

	// Create pipelines
	N_RETURN(createInputLayout(), false);
	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(backFormat, rtFormat, dsFormat, smFormat), false);

	return true;
}

bool ObjectRenderer::SetViewport(uint32_t width, uint32_t height, Format rtFormat,
	Format dsFormat, const float* clearColor)
{
	m_viewport = XMUINT2(width, height);

	// Recreate window size-dependent resource
	m_color = RenderTarget::MakeUnique();
	N_RETURN(m_color->Create(m_device.get(), width, height, rtFormat, 1,
		ResourceFlag::NONE, 1, 1, clearColor, false, L"RenderTarget"), false);

	m_depths[DEPTH_MAP] = DepthStencil::MakeUnique();
	N_RETURN(m_depths[DEPTH_MAP]->Create(m_device.get(), width, height, dsFormat,
		ResourceFlag::NONE, 1, 1, 1, 1.0f, 0, false, L"Depth"), false);

	return createDescriptorTables();
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

void ObjectRenderer::UpdateFrame(uint8_t frameIndex, CXMMATRIX viewProj, const XMFLOAT3& eyePt)
{
	XMFLOAT4X4 shadowWVP;
	const auto world = XMMatrixScaling(m_posScale.w, m_posScale.w, m_posScale.w) *
		XMMatrixTranslation(m_posScale.x, m_posScale.y, m_posScale.z);

	{
		const auto zNear = 1.0f;
		const auto zFar = 200.0f;

		const auto size = m_sceneSize * 1.5f;
		const auto lightPos = XMLoadFloat3(&m_lightPt);
		const auto lightView = XMMatrixLookAtLH(lightPos, XMVectorZero(), XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f));
		const auto lightProj = XMMatrixOrthographicLH(size, size, zNear, zFar);
		const auto lightViewProj = lightView * lightProj;
		XMStoreFloat4x4(&m_shadowVP, lightViewProj);
		XMStoreFloat4x4(&shadowWVP, XMMatrixTranspose(world * lightViewProj));

		const auto pCbData = reinterpret_cast<XMFLOAT4X4*>(m_cbShadow->Map(frameIndex));
		*pCbData = shadowWVP;
	}

	{
		const auto pCbData = reinterpret_cast<CBPerObject*>(m_cbPerObject->Map(frameIndex));
		XMStoreFloat4x4(&pCbData->WorldViewProj, XMMatrixTranspose(world * viewProj));
		XMStoreFloat3x4(&pCbData->World, world);
		pCbData->ShadowWVP = shadowWVP;
	}

	{
		const auto pCbData = reinterpret_cast<CBPerFrame*>(m_cbPerFrame->Map(frameIndex));
		pCbData->EyePos = XMFLOAT4(eyePt.x, eyePt.y, eyePt.z, 1.0f);
		pCbData->LightPos = XMFLOAT4(m_lightPt.x, m_lightPt.y, m_lightPt.z, 1.0f);
		pCbData->LightColor = m_lightColor;
		pCbData->Ambient = m_ambient;
	}

	if (m_lightProbes[RADIANCE_MAP])
	{
		const auto projToWorld = XMMatrixInverse(nullptr, viewProj);
		const auto pCbData = reinterpret_cast<CBPerFrameEnv*>(m_cbPerFrameEnv->Map(frameIndex));
		pCbData->EyePos = XMFLOAT4(eyePt.x, eyePt.y, eyePt.z, 1.0f);
		XMStoreFloat4x4(&pCbData->ScreenToWorld, XMMatrixTranspose(projToWorld));
	}
}

void ObjectRenderer::RenderShadow(const CommandList* pCommandList, uint8_t frameIndex, bool draw)
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

	if (draw)
	{
		// Set shadow viewport
		Viewport viewport(0.0f, 0.0f, static_cast<float>(m_shadowMapSize), static_cast<float>(m_shadowMapSize));
		RectRange scissorRect(0, 0, m_shadowMapSize, m_shadowMapSize);
		pCommandList->RSSetViewports(1, &viewport);
		pCommandList->RSSetScissorRects(1, &scissorRect);

		renderDepth(pCommandList, frameIndex, m_cbShadow.get());
	}
}

void ObjectRenderer::Render(const CommandList* pCommandList, uint8_t frameIndex)
{
	render(pCommandList, frameIndex);
	if (m_lightProbes[RADIANCE_MAP]) environment(pCommandList, frameIndex);
}

void ObjectRenderer::ToneMap(const CommandList* pCommandList)
{
	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[TONE_MAP]);
	pCommandList->SetPipelineState(m_pipelines[TONE_MAP]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Set descriptor tables
	pCommandList->SetGraphicsDescriptorTable(0, m_srvTables[SRV_TABLE_COLOR]);

	pCommandList->Draw(3, 1, 0, 0);
}

RenderTarget* ObjectRenderer::GetRenderTarget() const
{
	return m_color.get();
}

DepthStencil* ObjectRenderer::GetDepthMap(DepthIndex index) const
{
	return m_depths[index].get();
}

ShaderResource* ObjectRenderer::GetIrradiance() const
{
	return m_lightProbes[IRRADIANCE_MAP].get();
}

const DepthStencil::uptr* ObjectRenderer::GetDepthMaps() const
{
	return m_depths;
}

FXMMATRIX ObjectRenderer::GetShadowVP() const
{
	return XMLoadFloat4x4(&m_shadowVP);
}

bool ObjectRenderer::createVB(CommandList* pCommandList, uint32_t numVert,
	uint32_t stride, const uint8_t* pData, vector<Resource::uptr>& uploaders)
{
	m_vertexBuffer = VertexBuffer::MakeUnique();
	N_RETURN(m_vertexBuffer->Create(m_device.get(), numVert, stride, ResourceFlag::NONE,
		MemoryType::DEFAULT, 1, nullptr, 1, nullptr, 1, nullptr, L"MeshVB"), false);
	uploaders.emplace_back(Resource::MakeUnique());

	return m_vertexBuffer->Upload(pCommandList, uploaders.back().get(), pData, stride * numVert);
}

bool ObjectRenderer::createIB(CommandList* pCommandList, uint32_t numIndices,
	const uint32_t* pData, vector<Resource::uptr>& uploaders)
{
	m_numIndices = numIndices;

	const uint32_t byteWidth = sizeof(uint32_t) * numIndices;
	m_indexBuffer = IndexBuffer::MakeUnique();
	N_RETURN(m_indexBuffer->Create(m_device.get(), byteWidth, Format::R32_UINT, ResourceFlag::NONE,
		MemoryType::DEFAULT, 1, nullptr, 1, nullptr, 1, nullptr, L"MeshIB"), false);
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

	X_RETURN(m_pInputLayout, m_graphicsPipelineCache->CreateInputLayout(inputElements, static_cast<uint32_t>(size(inputElements))), false);

	return true;
}

bool ObjectRenderer::createPipelineLayouts()
{
	// Depth pass
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRootCBV(0, 0, 0, Shader::Stage::VS);
		X_RETURN(m_pipelineLayouts[DEPTH_PASS], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
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
		pipelineLayout->SetRange(2, DescriptorType::SRV, 3, 0);
		pipelineLayout->SetConstants(3, 1, 1, 0, Shader::Stage::PS);
		pipelineLayout->SetStaticSamplers(samplers, static_cast<uint32_t>(size(samplers)), 0, 0, Shader::PS);
		pipelineLayout->SetShaderStage(0, Shader::Stage::VS);
		pipelineLayout->SetShaderStage(1, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(2, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[BASE_PASS], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"BasePassLayout"), false);
	}

	// Environment mapping
	if (m_lightProbes[RADIANCE_MAP])
	{
		const auto sampler = m_descriptorTableCache->GetSampler(SamplerPreset::LINEAR_WRAP);

		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRootCBV(0, 0, 0, Shader::Stage::PS);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetShaderStage(1, Shader::Stage::PS);
		pipelineLayout->SetStaticSamplers(&sampler, 1, 0, 0, Shader::PS);
		X_RETURN(m_pipelineLayouts[ENVIRONMENT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"EnvironmentLayout"), false);
	}

	// Tone mapping
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[TONE_MAP], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"ToneMappingLayout"), false);
	}

	return true;
}

bool ObjectRenderer::createPipelines(Format backFormat, Format rtFormat, Format dsFormat, Format dsFormatH)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;

	// Depth pass
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSDepth.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[DEPTH_PASS]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->IASetInputLayout(m_pInputLayout);
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		//state->DSSetState(Graphics::DepthStencilPreset::DEFAULT_LESS, m_graphicsPipelineCache.get());
		state->OMSetDSVFormat(dsFormatH);
		X_RETURN(m_pipelines[DEPTH_PASS], state->GetPipeline(m_graphicsPipelineCache.get(), L"BasePass"), false);
	}

	// Base pass
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSBasePass.cso"), false);
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSBasePass.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[BASE_PASS]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetInputLayout(m_pInputLayout);
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		//state->DSSetState(Graphics::DepthStencilPreset::DEFAULT_LESS, m_graphicsPipelineCache.get());
		state->OMSetRTVFormats(&rtFormat, 1);
		state->OMSetDSVFormat(dsFormat);
		X_RETURN(m_pipelines[BASE_PASS], state->GetPipeline(m_graphicsPipelineCache.get(), L"BasePass"), false);
	}

	N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);

	// Environment mapping
	if (m_lightProbes[RADIANCE_MAP])
	{
		
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSEnvironment.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[ENVIRONMENT]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->DSSetState(Graphics::DEPTH_READ_LESS_EQUAL, m_graphicsPipelineCache.get());
		state->OMSetRTVFormats(&rtFormat, 1);
		state->OMSetDSVFormat(dsFormat);
		X_RETURN(m_pipelines[ENVIRONMENT], state->GetPipeline(m_graphicsPipelineCache.get(), L"Environment"), false);
	}

	// Tone mapping
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSToneMap.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[TONE_MAP]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->DSSetState(Graphics::DepthStencilPreset::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->OMSetRTVFormats(&backFormat, 1);
		X_RETURN(m_pipelines[TONE_MAP], state->GetPipeline(m_graphicsPipelineCache.get(), L"ToneMapping"), false);
	}

	return true;
}

bool ObjectRenderer::createDescriptorTables()
{
	// Create SRV tables
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_color->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_COLOR], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_depths[DEPTH_MAP]->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_DEPTH], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_depths[SHADOW_MAP]->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_SHADOW], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	if (m_lightProbes[IRRADIANCE_MAP])
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_lightProbes[IRRADIANCE_MAP]->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_IRRADIANCE], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	if (m_lightProbes[RADIANCE_MAP])
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_lightProbes[RADIANCE_MAP]->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_RADIANCE], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
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
	uint8_t hasLightProbes = m_lightProbes[IRRADIANCE_MAP] ? IRRADIANCE_BIT : 0;
	hasLightProbes |= m_lightProbes[RADIANCE_MAP] ? RADIANCE_BIT : 0;
	pCommandList->SetGraphicsRootConstantBufferView(0, m_cbPerObject.get(), m_cbPerObject->GetCBVOffset(frameIndex));
	pCommandList->SetGraphicsRootConstantBufferView(1, m_cbPerFrame.get(), m_cbPerFrame->GetCBVOffset(frameIndex));
	pCommandList->SetGraphicsDescriptorTable(2, m_srvTables[SRV_TABLE_SHADOW]);
	pCommandList->SetGraphics32BitConstant(3, hasLightProbes);
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

void ObjectRenderer::environment(const CommandList* pCommandList, uint8_t frameIndex)
{
	// Set descriptor tables
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[ENVIRONMENT]);
	pCommandList->SetGraphicsRootConstantBufferView(0, m_cbPerFrameEnv.get(), m_cbPerFrameEnv->GetCBVOffset(frameIndex));
	pCommandList->SetGraphicsDescriptorTable(1, m_srvTables[SRV_TABLE_RADIANCE]);

	// Set pipeline state
	pCommandList->SetPipelineState(m_pipelines[ENVIRONMENT]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);
	pCommandList->Draw(3, 1, 0, 0);
}
