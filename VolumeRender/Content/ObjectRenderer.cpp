//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Optional/XUSGObjLoader.h"
#include "ObjectRenderer.h"

using namespace std;
using namespace DirectX;
using namespace XUSG;

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

bool ObjectRenderer::Init(CommandList* pCommandList, uint32_t width, uint32_t height,
	const DescriptorTableCache::sptr& descriptorTableCache, vector<Resource::uptr>& uploaders,
	const char* fileName, Format rtFormat, Format dsFormat, const XMFLOAT4& posScale)
{
	m_descriptorTableCache = descriptorTableCache;
	m_posScale = posScale;

	// Load inputs
	ObjLoader objLoader;
	if (!objLoader.Import(fileName, true, true)) return false;
	N_RETURN(createVB(pCommandList, objLoader.GetNumVertices(), objLoader.GetVertexStride(), objLoader.GetVertices(), uploaders), false);
	N_RETURN(createIB(pCommandList, objLoader.GetNumIndices(), objLoader.GetIndices(), uploaders), false);
	m_sceneSize = objLoader.GetRadius() * posScale.w * 2.0f;

	// Create resources
	const auto smFormat = Format::D16_UNORM;
	for (auto& depth : m_depths) depth = DepthStencil::MakeUnique();
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

	// Create window size-dependent resource
	N_RETURN(SetViewport(width, height, dsFormat), false);

	// Create pipelines
	N_RETURN(createInputLayout(), false);
	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat, dsFormat, smFormat), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

bool ObjectRenderer::SetViewport(uint32_t width, uint32_t height, Format dsFormat)
{
	m_viewport = XMUINT2(width, height);

	// Recreate window size-dependent resource
	return m_depths[DEPTH_MAP]->Create(m_device.get(), width, height, dsFormat,
		ResourceFlag::NONE, 1, 1, 1, 1.0f, 0, false, L"Depth");
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
}

void ObjectRenderer::RenderShadow(const CommandList* pCommandList, uint8_t frameIndex)
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

	// Set shadow viewport
	Viewport viewport(0.0f, 0.0f, static_cast<float>(m_shadowMapSize), static_cast<float>(m_shadowMapSize));
	RectRange scissorRect(0, 0, m_shadowMapSize, m_shadowMapSize);
	pCommandList->RSSetViewports(1, &viewport);
	pCommandList->RSSetScissorRects(1, &scissorRect);

	renderDepth(pCommandList, frameIndex, m_cbShadow.get());
}

void ObjectRenderer::Render(const CommandList* pCommandList, uint8_t frameIndex)
{
	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[BASE_PASS]);
	pCommandList->SetPipelineState(m_pipelines[BASE_PASS]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Set descriptor tables
	pCommandList->SetGraphicsRootConstantBufferView(0, m_cbPerObject.get(), m_cbPerObject->GetCBVOffset(frameIndex));
	pCommandList->SetGraphicsRootConstantBufferView(1, m_cbPerFrame.get(), m_cbPerFrame->GetCBVOffset(frameIndex));

	pCommandList->IASetVertexBuffers(0, 1, &m_vertexBuffer->GetVBV());
	pCommandList->IASetIndexBuffer(m_indexBuffer->GetIBV());

	pCommandList->DrawIndexed(m_numIndices, 1, 0, 0, 0);
}

DepthStencil* ObjectRenderer::GetDepthMap(DepthIndex index) const
{
	return m_depths[index].get();
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
		pipelineLayout->SetRootCBV(0, 0, 0, Shader::VS);
		X_RETURN(m_pipelineLayouts[DEPTH_PASS], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"DepthPassLayout"), false);
	}

	// Base pass
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRootCBV(0, 0, 0, Shader::VS);
		pipelineLayout->SetRootCBV(1, 0, 0, Shader::PS);
		X_RETURN(m_pipelineLayouts[BASE_PASS], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"BasePassLayout"), false);
	}

	return true;
}

bool ObjectRenderer::createPipelines(Format rtFormat, Format dsFormat, XUSG::Format dsFormatH)
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

	return true;
}

bool ObjectRenderer::createDescriptorTables()
{
	return true;
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
