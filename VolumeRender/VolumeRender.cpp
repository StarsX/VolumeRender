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

#include "SharedConsts.h"
#include "VolumeRender.h"
#include "stb_image_write.h"
#include <DirectXColors.h>

using namespace std;
using namespace XUSG;

enum RenderMethod
{
	RAY_MARCH_MERGED,
	RAY_MARCH_SEPARATE,
	RAY_MARCH_DIRECT_MERGED,
	RAY_MARCH_DIRECT_SEPARATE,

	NUM_RENDER_METHOD
};

const float g_FOVAngleY = XM_PIDIV4;

RenderMethod g_renderMethod = RAY_MARCH_SEPARATE;
const auto g_backFormat = Format::R8G8B8A8_UNORM;
const auto g_rtFormat = Format::R16G16B16A16_FLOAT;
const auto g_dsFormat = Format::D32_FLOAT;

VolumeRender::VolumeRender(uint32_t width, uint32_t height, std::wstring name) :
	DXFramework(width, height, name),
	m_frameIndex(0),
	m_deviceType(DEVICE_DISCRETE),
	m_animate(false),
	m_showMesh(true),
	m_showFPS(true),
	m_isPaused(false),
	m_tracking(false),
	m_gridSize(128),
	m_lightGridSize(128),
	m_maxRaySamples(256),
	m_maxLightSamples(128),
	m_volumeFile(L"Assets/cloud2.dds"),
	m_radianceFile(L"Assets/Beach.dds"),
	m_meshFileName("Assets/dragon.obj"),
	m_volPosScale(0.0f, -4.0f, 0.0f, 14.0f),
	m_meshPosScale(0.0f, -4.0f, 0.0f, 1.4f),
	m_screenShot(0)
{
#if defined (_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	AllocConsole();
	FILE* stream;
	freopen_s(&stream, "CONIN$", "r+t", stdin);
	freopen_s(&stream, "CONOUT$", "w+t", stdout);
	freopen_s(&stream, "CONOUT$", "w+t", stderr);
#endif
}

VolumeRender::~VolumeRender()
{
#if defined (_DEBUG)
	FreeConsole();
#endif
}

void VolumeRender::OnInit()
{
	LoadPipeline();
	LoadAssets();
}

// Load the rendering pipeline dependencies.
void VolumeRender::LoadPipeline()
{
	auto dxgiFactoryFlags = 0u;

#if defined(_DEBUG)
	// Enable the debug layer (requires the Graphics Tools "optional feature").
	// NOTE: Enabling the debug layer after device creation will invalidate the active device.
	{
		com_ptr<ID3D12Debug1> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();
			//debugController->SetEnableGPUBasedValidation(TRUE);

			// Enable additional debug layers.
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
		}
	}
#endif

	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_factory)));

	DXGI_ADAPTER_DESC1 dxgiAdapterDesc;
	com_ptr<IDXGIAdapter1> dxgiAdapter;
	const auto useUMA = m_deviceType == DEVICE_UMA;
	const auto useWARP = m_deviceType == DEVICE_WARP;
	auto checkUMA = true, checkWARP = true;
	auto hr = DXGI_ERROR_NOT_FOUND;
	for (uint8_t n = 0; n < 3; ++n)
	{
		if (FAILED(hr)) hr = DXGI_ERROR_UNSUPPORTED;
		for (auto i = 0u; hr == DXGI_ERROR_UNSUPPORTED; ++i)
		{
			dxgiAdapter = nullptr;
			hr = m_factory->EnumAdapters1(i, &dxgiAdapter);

			if (SUCCEEDED(hr) && dxgiAdapter)
			{
				dxgiAdapter->GetDesc1(&dxgiAdapterDesc);
				if (checkWARP) hr = dxgiAdapterDesc.VendorId == 0x1414 && dxgiAdapterDesc.DeviceId == 0x8c ?
					(useWARP ? hr : DXGI_ERROR_UNSUPPORTED) : (useWARP ? DXGI_ERROR_UNSUPPORTED : hr);
			}

			if (SUCCEEDED(hr))
			{
				m_device = Device::MakeUnique();
				if (SUCCEEDED(m_device->Create(dxgiAdapter.get(), D3D_FEATURE_LEVEL_11_0)) && checkUMA)
				{
					D3D12_FEATURE_DATA_ARCHITECTURE feature = {};
					const auto pDevice = static_cast<ID3D12Device*>(m_device->GetHandle());
					if (SUCCEEDED(pDevice->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &feature, sizeof(feature))))
						hr = feature.UMA ? (useUMA ? hr : DXGI_ERROR_UNSUPPORTED) : (useUMA ? DXGI_ERROR_UNSUPPORTED : hr);
				}
			}
		}

		checkUMA = false;
		if (n) checkWARP = false;
	}

	if (dxgiAdapterDesc.VendorId == 0x1414 && dxgiAdapterDesc.DeviceId == 0x8c) m_title += L" (WARP)";
	else if (dxgiAdapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) m_title += L" (Software)";
	//else m_title += wstring(L" - ") + dxgiAdapterDesc.Description;
	ThrowIfFailed(hr);

	// Create the command queue.
	m_commandQueue = CommandQueue::MakeUnique();
	XUSG_N_RETURN(m_commandQueue->Create(m_device.get(), CommandListType::DIRECT, CommandQueueFlag::NONE,
		0, 0, L"CommandQueue"), ThrowIfFailed(E_FAIL));

	// Create the swap chain.
	CreateSwapchain();

	// Reset the index to the current back buffer.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// Create a command allocator for each frame.
	for (uint8_t n = 0; n < FrameCount; ++n)
	{
		m_commandAllocators[n] = CommandAllocator::MakeUnique();
		XUSG_N_RETURN(m_commandAllocators[n]->Create(m_device.get(), CommandListType::DIRECT,
			(L"CommandAllocator" + to_wstring(n)).c_str()), ThrowIfFailed(E_FAIL));
	}

	// Create descriptor-table lib.
	m_descriptorTableLib = DescriptorTableLib::MakeShared(m_device.get(), L"DescriptorTableLib");
}

// Load the sample assets.
void VolumeRender::LoadAssets()
{
	// Create the command list.
	m_commandList = CommandList::MakeUnique();
	const auto pCommandList = m_commandList.get();
	XUSG_N_RETURN(pCommandList->Create(m_device.get(), 0, CommandListType::DIRECT,
		m_commandAllocators[m_frameIndex].get(), nullptr), ThrowIfFailed(E_FAIL));

	// Clear color setting
	m_clearColor = { 0.2f, 0.2f, 0.2f, 0.0f };
	m_clearColor = m_volumeFile.empty() ? m_clearColor : DirectX::Colors::CornflowerBlue;
	m_clearColor.v = XMVectorPow(m_clearColor, XMVectorReplicate(1.0f / 1.25f));
	m_clearColor.v = 0.7f * m_clearColor / (XMVectorReplicate(1.25f) - m_clearColor);
	m_clearColor.f[3] = 0.0f;

	// Init assets
	vector<Resource::uptr> uploaders(0);
	if (!m_radianceFile.empty())
	{
		XUSG_X_RETURN(m_lightProbe, make_unique<LightProbe>(), ThrowIfFailed(E_FAIL));
		XUSG_N_RETURN(m_lightProbe->Init(pCommandList, m_descriptorTableLib, uploaders,
			m_radianceFile.c_str(), g_rtFormat, g_dsFormat), ThrowIfFailed(E_FAIL));
	}

	XUSG_X_RETURN(m_objectRenderer, make_unique<ObjectRenderer>(), ThrowIfFailed(E_FAIL));
	XUSG_N_RETURN(m_objectRenderer->Init(m_commandList.get(), m_descriptorTableLib, uploaders,
		m_meshFileName.c_str(), g_backFormat, g_rtFormat, g_dsFormat, m_meshPosScale), ThrowIfFailed(E_FAIL));

	XUSG_X_RETURN(m_rayCaster, make_unique<RayCaster>(), ThrowIfFailed(E_FAIL));
	XUSG_N_RETURN(m_rayCaster->Init(m_device.get(), m_descriptorTableLib, g_rtFormat, m_gridSize,
		m_lightGridSize, m_objectRenderer->GetDepthMaps()), ThrowIfFailed(E_FAIL));
	const auto volumeSize = m_volPosScale.w * 2.0f;
	const auto volumePos = XMFLOAT3(m_volPosScale.x, m_volPosScale.y, m_volPosScale.z);
	m_rayCaster->SetVolumeWorld(volumeSize, volumePos);
	m_rayCaster->SetMaxSamples(m_maxRaySamples, m_maxLightSamples);

	if (m_volumeFile.empty()) m_rayCaster->InitVolumeData(pCommandList);
	else m_rayCaster->LoadVolumeData(pCommandList, m_volumeFile.c_str(), uploaders);

	// Close the command list and execute it to begin the initial GPU setup.
	XUSG_N_RETURN(pCommandList->Close(), ThrowIfFailed(E_FAIL));
	m_commandQueue->ExecuteCommandList(pCommandList);

	// Create synchronization objects and wait until assets have been uploaded to the GPU.
	{
		if (!m_fence)
		{
			m_fence = Fence::MakeUnique();
			XUSG_N_RETURN(m_fence->Create(m_device.get(), m_fenceValues[m_frameIndex]++, FenceFlag::NONE, L"Fence"), ThrowIfFailed(E_FAIL));
		}

		// Create an event handle to use for frame synchronization.
		m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!m_fenceEvent) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));

		// Wait for the command list to execute; we are reusing the same command 
		// list in our main loop but for now, we just want to wait for setup to 
		// complete before continuing.
		WaitForGpu();
	}

	// Create window size dependent resources.
	//m_descriptorTableCache->ResetDescriptorPool(CBV_SRV_UAV_POOL, 0);
	CreateResources();

	// Projection
	{
		const auto aspectRatio = m_width / static_cast<float>(m_height);
		const auto proj = XMMatrixPerspectiveFovLH(g_FOVAngleY, aspectRatio, g_zNear, g_zFar);
		XMStoreFloat4x4(&m_proj, proj);
	}

	// View initialization
	{
		m_focusPt = XMFLOAT3(0.0f, 0.0f, 0.0f);
		m_eyePt = XMFLOAT3(4.0f, 16.0f, -40.0f);
		const auto focusPt = XMLoadFloat3(&m_focusPt);
		const auto eyePt = XMLoadFloat3(&m_eyePt);
		const auto view = XMMatrixLookAtLH(eyePt, focusPt, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
		XMStoreFloat4x4(&m_view, view);
	}
}

void VolumeRender::CreateSwapchain()
{
	// Describe and create the swap chain.
	m_swapChain = SwapChain::MakeUnique();
	XUSG_N_RETURN(m_swapChain->Create(m_factory.get(), Win32Application::GetHwnd(), m_commandQueue->GetHandle(),
		FrameCount, m_width, m_height, g_backFormat, SwapChainFlag::ALLOW_TEARING), ThrowIfFailed(E_FAIL));

	// This class does not support exclusive full-screen mode and prevents DXGI from responding to the ALT+ENTER shortcut.
	ThrowIfFailed(m_factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));
}

void VolumeRender::CreateResources()
{
	// Obtain the back buffers for this window which will be the final render targets
	// and create render target views for each of them.
	for (uint8_t n = 0; n < FrameCount; ++n)
	{
		m_renderTargets[n] = RenderTarget::MakeUnique();
		XUSG_N_RETURN(m_renderTargets[n]->CreateFromSwapChain(m_device.get(), m_swapChain.get(), n), ThrowIfFailed(E_FAIL));
	}

	if (m_lightProbe)
	{
		XUSG_N_RETURN(m_lightProbe->CreateDescriptorTables(m_device.get()), ThrowIfFailed(E_FAIL));
		XUSG_N_RETURN(m_objectRenderer->SetRadiance(m_lightProbe->GetRadiance()->GetSRV()), ThrowIfFailed(E_FAIL));
	}
	XUSG_N_RETURN(m_objectRenderer->SetViewport(m_device.get(), m_width, m_height, g_rtFormat, g_dsFormat, m_clearColor), ThrowIfFailed(E_FAIL));
	XUSG_N_RETURN(m_rayCaster->SetDepthMaps(m_objectRenderer->GetDepthMaps()), ThrowIfFailed(E_FAIL));
}

// Update frame-based values.
void VolumeRender::OnUpdate()
{
	// Timer
	static auto time = 0.0, pauseTime = 0.0;

	m_timer.Tick();
	float timeStep;
	const auto totalTime = CalculateFrameStats(&timeStep);
	pauseTime = m_isPaused ? totalTime - time : pauseTime;
	timeStep = m_isPaused ? 0.0f : timeStep;
	time = totalTime - pauseTime;

	// Set animations
	if (m_animate)
	{
		const auto meshPitchYawRoll = XMFLOAT3(0.0f, static_cast<float>(time), 0.0f);
		const auto volPitchYawRoll = XMFLOAT3(0.0f, -static_cast<float>(time), 0.0f);
		m_objectRenderer->SetWorld(m_meshPosScale.w, XMFLOAT3(m_meshPosScale.x, m_meshPosScale.y, m_meshPosScale.z), &meshPitchYawRoll);
		m_rayCaster->SetVolumeWorld(m_volPosScale.w * 2.0f, XMFLOAT3(m_volPosScale.x, m_volPosScale.y, m_volPosScale.z), &volPitchYawRoll);
	}

	// Set lighting
	const XMFLOAT3 lightPt(75.0f, 75.0f, -75.0f);
	const XMFLOAT3 lightColor(1.0f, 0.7f, 0.3f);
	const XMFLOAT3 ambientColor(0.4f, 0.6f, 1.0f);
	const auto lightIntensity = 3.0f * XM_PI, ambientIntensity = 2.0f * XM_PI;
	m_objectRenderer->SetLight(lightPt, lightColor, lightIntensity);
	m_objectRenderer->SetAmbient(ambientColor, ambientIntensity);
	m_rayCaster->SetLight(lightPt, lightColor, lightIntensity);
	m_rayCaster->SetAmbient(ambientColor, ambientIntensity);

	// View
	//const auto eyePt = XMLoadFloat3(&m_eyePt);
	const auto view = XMLoadFloat4x4(&m_view);
	const auto proj = XMLoadFloat4x4(&m_proj);
	const auto viewProj = view * proj;
	if (m_lightProbe) m_lightProbe->UpdateFrame(m_frameIndex, viewProj, m_eyePt);
	m_objectRenderer->UpdateFrame(m_frameIndex, viewProj, m_eyePt);
	m_rayCaster->UpdateFrame(m_frameIndex, viewProj, m_objectRenderer->GetShadowVP(), m_eyePt);
}

// Render the scene.
void VolumeRender::OnRender()
{
	// Record all the commands we need to render the scene into the command list.
	PopulateCommandList();

	// Execute the command list.
	m_commandQueue->ExecuteCommandList(m_commandList.get());

	// Present the frame.
	XUSG_N_RETURN(m_swapChain->Present(0, PresentFlag::ALLOW_TEARING), ThrowIfFailed(E_FAIL));

	MoveToNextFrame();
}

void VolumeRender::OnDestroy()
{
	// Ensure that the GPU is no longer referencing resources that are about to be
	// cleaned up by the destructor.
	WaitForGpu();

	CloseHandle(m_fenceEvent);
}

void VolumeRender::OnWindowSizeChanged(int width, int height)
{
	if (!Win32Application::GetHwnd())
	{
		throw std::exception("Call SetWindow with a valid Win32 window handle");
	}

	// Wait until all previous GPU work is complete.
	WaitForGpu();

	// Release resources that are tied to the swap chain and update fence values.
	for (uint8_t n = 0; n < FrameCount; ++n)
	{
		m_renderTargets[n].reset();
		m_fenceValues[n] = m_fenceValues[m_frameIndex];
	}
	m_descriptorTableLib->ResetDescriptorHeap(CBV_SRV_UAV_HEAP);
	//m_descriptorTableCache->ResetDescriptorHeap(RTV_HEAP);

	// Determine the render target size in pixels.
	m_width = (max)(width, 1);
	m_height = (max)(height, 1);

	// If the swap chain already exists, resize it, otherwise create one.
	if (m_swapChain)
	{
		// If the swap chain already exists, resize it.
		const auto hr = m_swapChain->ResizeBuffers(FrameCount, m_width, m_height, g_backFormat, SwapChainFlag::ALLOW_TEARING);

		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		{
#ifdef _DEBUG
			char buff[64] = {};
			sprintf_s(buff, "Device Lost on ResizeBuffers: Reason code 0x%08X\n", (hr == DXGI_ERROR_DEVICE_REMOVED) ? m_device->GetDeviceRemovedReason() : hr);
			OutputDebugStringA(buff);
#endif
			// If the device was removed for any reason, a new device and swap chain will need to be created.
			//HandleDeviceLost();

			// Everything is set up now. Do not continue execution of this method. HandleDeviceLost will reenter this method 
			// and correctly set up the new device.
			return;
		}
		else
		{
			ThrowIfFailed(hr);
		}
	}
	else CreateSwapchain();

	// Reset the index to the current back buffer.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// Create window size dependent resources.
	CreateResources();

	// Projection
	{
		const auto aspectRatio = m_width / static_cast<float>(m_height);
		const auto proj = XMMatrixPerspectiveFovLH(g_FOVAngleY, aspectRatio, g_zNear, g_zFar);
		XMStoreFloat4x4(&m_proj, proj);
	}
}

// User hot-key interactions.
void VolumeRender::OnKeyUp(uint8_t key)
{
	switch (key)
	{
	case VK_SPACE:
		m_isPaused = !m_isPaused;
		break;
	case VK_F1:
		m_showFPS = !m_showFPS;
		break;
	case VK_LEFT:
		g_renderMethod = static_cast<RenderMethod>((g_renderMethod + NUM_RENDER_METHOD - 1) % NUM_RENDER_METHOD);
		break;
	case VK_RIGHT:
		g_renderMethod = static_cast<RenderMethod>((g_renderMethod + 1) % NUM_RENDER_METHOD);
		break;
	case VK_F11:
		m_screenShot = 1;
		break;
	case 'A':
		m_animate = !m_animate;
		break;
	case 'M':
		m_showMesh = m_meshFileName.empty() ? false : !m_showMesh;
		break;
	}
}

// User camera interactions.
void VolumeRender::OnLButtonDown(float posX, float posY)
{
	m_tracking = true;
	m_mousePt = XMFLOAT2(posX, posY);
}

void VolumeRender::OnLButtonUp(float posX, float posY)
{
	m_tracking = false;
}

void VolumeRender::OnMouseMove(float posX, float posY)
{
	if (m_tracking)
	{
		const auto dPos = XMFLOAT2(m_mousePt.x - posX, m_mousePt.y - posY);

		XMFLOAT2 radians;
		radians.x = XM_2PI * dPos.y / m_height;
		radians.y = XM_2PI * dPos.x / m_width;

		const auto focusPt = XMLoadFloat3(&m_focusPt);
		auto eyePt = XMLoadFloat3(&m_eyePt);

		const auto len = XMVectorGetX(XMVector3Length(focusPt - eyePt));
		auto transform = XMMatrixTranslation(0.0f, 0.0f, -len);
		transform *= XMMatrixRotationRollPitchYaw(radians.x, radians.y, 0.0f);
		transform *= XMMatrixTranslation(0.0f, 0.0f, len);

		const auto view = XMLoadFloat4x4(&m_view) * transform;
		const auto viewInv = XMMatrixInverse(nullptr, view);
		eyePt = viewInv.r[3];

		XMStoreFloat3(&m_eyePt, eyePt);
		XMStoreFloat4x4(&m_view, view);

		m_mousePt = XMFLOAT2(posX, posY);
	}
}

void VolumeRender::OnMouseWheel(float deltaZ, float posX, float posY)
{
	const auto focusPt = XMLoadFloat3(&m_focusPt);
	auto eyePt = XMLoadFloat3(&m_eyePt);

	const auto len = XMVectorGetX(XMVector3Length(focusPt - eyePt));
	const auto transform = XMMatrixTranslation(0.0f, 0.0f, -len * deltaZ / 16.0f);

	const auto view = XMLoadFloat4x4(&m_view) * transform;
	const auto viewInv = XMMatrixInverse(nullptr, view);
	eyePt = viewInv.r[3];

	XMStoreFloat3(&m_eyePt, eyePt);
	XMStoreFloat4x4(&m_view, view);
}

void VolumeRender::OnMouseLeave()
{
	m_tracking = false;
}

void VolumeRender::ParseCommandLineArgs(wchar_t* argv[], int argc)
{
	DXFramework::ParseCommandLineArgs(argv, argc);

	for (auto i = 1; i < argc; ++i)
	{
		if (wcsncmp(argv[i], L"-warp", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/warp", wcslen(argv[i])) == 0)
			m_deviceType = DEVICE_WARP;
		else if (wcsncmp(argv[i], L"-uma", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/uma", wcslen(argv[i])) == 0)
			m_deviceType = DEVICE_UMA;
		else if (wcsncmp(argv[i], L"-mesh", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/mesh", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc)
			{
				m_meshFileName.resize(wcslen(argv[++i]));
				for (size_t j = 0; j < m_meshFileName.size(); ++j)
					m_meshFileName[j] = static_cast<char>(argv[i][j]);
			}
			if (!m_meshFileName.empty())
			{
				if (i + 1 < argc) m_meshPosScale.x = stof(argv[++i]);
				if (i + 1 < argc) m_meshPosScale.y = stof(argv[++i]);
				if (i + 1 < argc) m_meshPosScale.z = stof(argv[++i]);
				if (i + 1 < argc) m_meshPosScale.w = stof(argv[++i]);
			}
			m_showMesh = !m_meshFileName.empty();
		}
		else if (wcsncmp(argv[i], L"-gridSize", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/gridSize", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_gridSize = stoul(argv[++i]);
		}
		else if (wcsncmp(argv[i], L"-lightGridSize", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/lightGridSize", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_lightGridSize = stoul(argv[++i]);
		}
		else if (wcsncmp(argv[i], L"-volume", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/volume", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_volumeFile = argv[++i];
			if (i + 1 < argc) m_volPosScale.x = stof(argv[++i]);
			if (i + 1 < argc) m_volPosScale.y = stof(argv[++i]);
			if (i + 1 < argc) m_volPosScale.z = stof(argv[++i]);
			if (i + 1 < argc) m_volPosScale.w = stof(argv[++i]);
		}
		else if (wcsncmp(argv[i], L"-maxRaySamples", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/maxRaySamples", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_maxRaySamples = stoul(argv[++i]);
		}
		else if (wcsncmp(argv[i], L"-maxLightSamples", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/maxLightSamples", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_maxLightSamples = stoul(argv[++i]);
		}
		else if (wcsncmp(argv[i], L"-radiance", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/radiance", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_radianceFile = argv[++i];
		}
	}
}

void VolumeRender::PopulateCommandList()
{
	// Command list allocators can only be reset when the associated 
	// command lists have finished execution on the GPU; apps should use 
	// fences to determine GPU execution progress.
	const auto pCommandAllocator = m_commandAllocators[m_frameIndex].get();
	XUSG_N_RETURN(pCommandAllocator->Reset(), ThrowIfFailed(E_FAIL));

	// However, when ExecuteCommandList() is called on a particular command 
	// list, that command list can then be reset at any time and must be before 
	// re-recording.
	const auto pCommandList = m_commandList.get();
	XUSG_N_RETURN(pCommandList->Reset(pCommandAllocator, nullptr), ThrowIfFailed(E_FAIL));

	// Record commands.
	const auto descriptorHeap = m_descriptorTableLib->GetDescriptorHeap(CBV_SRV_UAV_HEAP);
	pCommandList->SetDescriptorHeaps(1, &descriptorHeap);

	if (m_lightProbe)
	{
		static auto isFirstFrame = true;
		if (isFirstFrame)
		{
			m_lightProbe->TransformSH(pCommandList);
			m_objectRenderer->SetSH(m_lightProbe->GetSH());
			m_rayCaster->SetSH(m_lightProbe->GetSH());
			isFirstFrame = false;
		}
	}

	m_objectRenderer->RenderShadow(pCommandList, m_frameIndex, m_showMesh);

	ResourceBarrier barriers[4];
	const auto pColor = m_objectRenderer->GetRenderTarget(ObjectRenderer::RT_COLOR);
	const auto pVelocity = m_objectRenderer->GetRenderTarget(ObjectRenderer::RT_VELOCITY);
	const auto pDepth = m_objectRenderer->GetDepthMap(ObjectRenderer::DEPTH_MAP);
	const auto pShadow = m_objectRenderer->GetDepthMap(ObjectRenderer::SHADOW_MAP);
	auto numBarriers = pColor->SetBarrier(barriers, ResourceState::RENDER_TARGET);
	numBarriers = pVelocity->SetBarrier(barriers, ResourceState::RENDER_TARGET, numBarriers);
	numBarriers = pDepth->SetBarrier(barriers, ResourceState::DEPTH_WRITE, numBarriers);
	numBarriers = pShadow->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE | ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);

	// Clear render targets
	const float clear[4] = {};
	const Descriptor pRTVs[] = { pColor->GetRTV(), pVelocity->GetRTV() };
	pCommandList->ClearRenderTargetView(pColor->GetRTV(), m_clearColor);
	pCommandList->ClearRenderTargetView(pVelocity->GetRTV(), clear);
	pCommandList->ClearDepthStencilView(pDepth->GetDSV(), ClearFlag::DEPTH, 1.0f);
	pCommandList->OMSetRenderTargets(static_cast<uint32_t>(size(pRTVs)), pRTVs, &pDepth->GetDSV());

	// Set viewport
	Viewport viewport(0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));
	RectRange scissorRect(0, 0, m_width, m_height);
	pCommandList->RSSetViewports(1, &viewport);
	pCommandList->RSSetScissorRects(1, &scissorRect);

	m_objectRenderer->Render(pCommandList, m_frameIndex, m_showMesh);
	if (m_lightProbe) m_lightProbe->RenderEnvironment(pCommandList, m_frameIndex);

	switch (g_renderMethod)
	{
	case RAY_MARCH_MERGED:
		m_rayCaster->Render(pCommandList, m_frameIndex, RayCaster::RAY_MARCH_CUBEMAP);
		break;
	case RAY_MARCH_SEPARATE:
		m_rayCaster->Render(pCommandList, m_frameIndex, RayCaster::OPTIMIZED);
		break;
	case RAY_MARCH_DIRECT_MERGED:
		m_rayCaster->Render(pCommandList, m_frameIndex, RayCaster::RAY_MARCH_DIRECT);
		break;
	case RAY_MARCH_DIRECT_SEPARATE:
		m_rayCaster->Render(pCommandList, m_frameIndex, RayCaster::SEPARATE_LIGHT_PASS);
		break;
	default:
		assert(!"Cannot reach here!");
	}

	const auto pRenderTarget = m_renderTargets[m_frameIndex].get();
	m_objectRenderer->Postprocess(pCommandList, pRenderTarget);

	// Indicate that the back buffer will now be used to present.
	numBarriers = pRenderTarget->SetBarrier(barriers, ResourceState::PRESENT);
	pCommandList->Barrier(numBarriers, barriers);

	// Screen-shot helper
	if (m_screenShot == 1)
	{
		if (!m_readBuffer) m_readBuffer = Buffer::MakeUnique();
		pRenderTarget->ReadBack(pCommandList, m_readBuffer.get(), &m_rowPitch);
		m_screenShot = 2;
	}

	XUSG_N_RETURN(pCommandList->Close(), ThrowIfFailed(E_FAIL));
}

// Wait for pending GPU work to complete.
void VolumeRender::WaitForGpu()
{
	// Schedule a Signal command in the queue.
	XUSG_N_RETURN(m_commandQueue->Signal(m_fence.get(), m_fenceValues[m_frameIndex]), ThrowIfFailed(E_FAIL));

	// Wait until the fence has been processed.
	XUSG_N_RETURN(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent), ThrowIfFailed(E_FAIL));
	WaitForSingleObject(m_fenceEvent, INFINITE);

	// Increment the fence value for the current frame.
	m_fenceValues[m_frameIndex]++;
}

// Prepare to render the next frame.
void VolumeRender::MoveToNextFrame()
{
	// Schedule a Signal command in the queue.
	const auto currentFenceValue = m_fenceValues[m_frameIndex];
	XUSG_N_RETURN(m_commandQueue->Signal(m_fence.get(), currentFenceValue), ThrowIfFailed(E_FAIL));

	// Update the frame index.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// If the next frame is not ready to be rendered yet, wait until it is ready.
	if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
	{
		XUSG_N_RETURN(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent), ThrowIfFailed(E_FAIL));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}

	// Set the fence value for the next frame.
	m_fenceValues[m_frameIndex] = currentFenceValue + 1;

	// Screen-shot helper
	if (m_screenShot)
	{
		if (m_screenShot > FrameCount)
		{
			char timeStr[15];
			tm dateTime;
			const auto now = time(nullptr);
			if (!localtime_s(&dateTime, &now) && strftime(timeStr, sizeof(timeStr), "%Y%m%d%H%M%S", &dateTime))
				SaveImage((string("VolumeRender_") + timeStr + ".png").c_str(), m_readBuffer.get(), m_width, m_height, m_rowPitch);
			m_screenShot = 0;
		}
		else ++m_screenShot;
	}
}

void VolumeRender::SaveImage(char const* fileName, Buffer* pImageBuffer, uint32_t w, uint32_t h, uint32_t rowPitch, uint8_t comp)
{
	assert(comp == 3 || comp == 4);
	const auto pData = static_cast<const uint8_t*>(pImageBuffer->Map(nullptr));

	//stbi_write_png_compression_level = 1024;
	vector<uint8_t> imageData(comp * w * h);
	const auto sw = rowPitch / 4; // Byte to pixel
	for (auto i = 0u; i < h; ++i)
		for (auto j = 0u; j < w; ++j)
		{
			const auto s = sw * i + j;
			const auto d = w * i + j;
			for (uint8_t k = 0; k < comp; ++k)
				imageData[comp * d + k] = pData[4 * s + k];
		}

	stbi_write_png(fileName, w, h, comp, imageData.data(), 0);

	pImageBuffer->Unmap();
}

double VolumeRender::CalculateFrameStats(float* pTimeStep)
{
	static auto frameCnt = 0u;
	static auto previousTime = 0.0;
	const auto totalTime = m_timer.GetTotalSeconds();
	++frameCnt;

	const auto timeStep = totalTime - previousTime;

	// Compute averages over one second period.
	if (timeStep >= 1.0)
	{
		const auto fps = static_cast<float>(frameCnt / timeStep);	// Normalize to an exact second.

		frameCnt = 0;
		previousTime = totalTime;

		wstringstream windowText;
		windowText << L"    fps: ";
		if (m_showFPS) windowText << setprecision(2) << fixed << fps;
		else windowText << L"[F1]";

		windowText << L"    [A] " << (m_animate ? "Auto-animation" : "Interaction");
		windowText << L"    [M] Show/hide mesh";
		windowText << L"    [\x2190][\x2192] ";
		switch (g_renderMethod)
		{
		case RAY_MARCH_MERGED:
			windowText << L"Cubemap-space ray marching without separate lighting pass";
			break;
		case RAY_MARCH_SEPARATE:
			windowText << L"Cubemap-space ray marching with separate lighting pass";
			break;
		case RAY_MARCH_DIRECT_MERGED:
			windowText << L"Direct screen-space ray marching without separate lighting pass";
			break;
		case RAY_MARCH_DIRECT_SEPARATE:
			windowText << L"Direct screen-space ray marching with separate lighting pass";
			break;
		}

		windowText << L"    [F11] screen shot";

		SetCustomWindowText(windowText.str().c_str());
	}

	if (pTimeStep) *pTimeStep = static_cast<float>(m_timer.GetElapsedSeconds());

	return totalTime;
}
