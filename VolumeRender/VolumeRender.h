//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#pragma once

#include "DXFramework.h"
#include "StepTimer.h"
#include "RayCaster.h"
#include "LightProbe.h"
#include "ObjectRenderer.h"

using namespace DirectX;

// Note that while ComPtr is used to manage the lifetime of resources on the CPU,
// it has no understanding of the lifetime of resources on the GPU. Apps must account
// for the GPU lifetime of resources to avoid destroying objects that may still be
// referenced by the GPU.
// An example of this can be found in the class method: OnDestroy().

class VolumeRender : public DXFramework
{
public:
	VolumeRender(uint32_t width, uint32_t height, std::wstring name);
	virtual ~VolumeRender();

	virtual void OnInit();
	virtual void OnUpdate();
	virtual void OnRender();
	virtual void OnDestroy();

	virtual void OnWindowSizeChanged(int width, int height);

	virtual void OnKeyUp(uint8_t /*key*/);
	virtual void OnLButtonDown(float posX, float posY);
	virtual void OnLButtonUp(float posX, float posY);
	virtual void OnMouseMove(float posX, float posY);
	virtual void OnMouseWheel(float deltaZ, float posX, float posY);
	virtual void OnMouseLeave();

	virtual void ParseCommandLineArgs(wchar_t* argv[], int argc);

private:
	static const auto FrameCount = RayCaster::FrameCount;
	static_assert(FrameCount == ObjectRenderer::FrameCount, "VolumeRender::FrameCount should be equal to ObjectRenderer::FrameCount");

	XUSG::com_ptr<IDXGIFactory4> m_factory;

	std::shared_ptr<XUSG::DescriptorTableCache> m_descriptorTableCache;

	XUSG::SwapChain::uptr			m_swapChain;
	XUSG::CommandAllocator::uptr	m_commandAllocators[FrameCount];
	XUSG::CommandQueue::uptr		m_commandQueue;

	XUSG::Device::sptr			m_device;
	XUSG::RenderTarget::uptr	m_renderTargets[FrameCount];
	XUSG::CommandList::uptr		m_commandList;

	// App resources
	std::unique_ptr<RayCaster>	m_rayCaster;
	std::unique_ptr<LightProbe>	m_lightProbe;
	std::unique_ptr<ObjectRenderer> m_objectRenderer;
	XMFLOAT4X4	m_proj;
	XMFLOAT4X4	m_view;
	XMFLOAT3	m_focusPt;
	XMFLOAT3	m_eyePt;

	// Synchronization objects.
	uint32_t	m_frameIndex;
	HANDLE		m_fenceEvent;
	XUSG::Fence::uptr m_fence;
	uint64_t	m_fenceValues[FrameCount];

	// Application state
	bool		m_animate;
	bool		m_showMesh;
	bool		m_showFPS;
	bool		m_isPaused;
	StepTimer	m_timer;
	
	// User camera interactions
	bool m_tracking;
	XMFLOAT2 m_mousePt;

	// User external settings
	uint32_t m_gridSize;
	uint32_t m_lightGridSize;
	uint32_t m_maxRaySamples;
	uint32_t m_maxLightSamples;
	uint32_t m_numParticles;
	float m_particleSize;
	std::wstring m_volumeFile;
	std::wstring m_radianceFile;
	std::string m_meshFileName;
	XMFLOAT4 m_volPosScale;
	XMFLOAT4 m_meshPosScale;
	XMVECTORF32 m_clearColor;

	void LoadPipeline();
	void LoadAssets();
	void CreateSwapchain();
	void CreateResources();
	void PopulateCommandList();
	void WaitForGpu();
	void MoveToNextFrame();
	double CalculateFrameStats(float* fTimeStep = nullptr);
};
