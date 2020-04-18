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

#include "VolumeRender.h"

using namespace std;
using namespace XUSG;

enum RenderMethod
{
	RAY_MARCH_MERGED,
	RAY_MARCH_SPLITTED,
	PARTICLE_OIT,
	PARTICLE_SIMPLE,

	NUM_RENDER_METHOD
};

const float g_FOVAngleY = XM_PIDIV4;
const float g_zNear = 1.0f;
const float g_zFar = 1000.0f;

RenderMethod g_renderMethod = RAY_MARCH_SPLITTED;

VolumeRender::VolumeRender(uint32_t width, uint32_t height, std::wstring name) :
	DXFramework(width, height, name),
	m_frameIndex(0),
	m_showFPS(true),
	m_isPaused(false),
	m_tracking(false),
	m_gridSize(128, 128, 128),
	m_numParticles(1 << 14),
	m_particleSize(2.5f),
	m_volumeFile(L"")
{
#if defined (_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	AllocConsole();
	FILE* stream;
	freopen_s(&stream, "CONOUT$", "w+t", stdout);
	freopen_s(&stream, "CONIN$", "r+t", stdin);
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

	com_ptr<IDXGIFactory4> factory;
	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

	DXGI_ADAPTER_DESC1 dxgiAdapterDesc;
	com_ptr<IDXGIAdapter1> dxgiAdapter;
	auto hr = DXGI_ERROR_UNSUPPORTED;
	for (auto i = 0u; hr == DXGI_ERROR_UNSUPPORTED; ++i)
	{
		dxgiAdapter = nullptr;
		ThrowIfFailed(factory->EnumAdapters1(i, &dxgiAdapter));
		hr = D3D12CreateDevice(dxgiAdapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
	}

	dxgiAdapter->GetDesc1(&dxgiAdapterDesc);
	if (dxgiAdapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
		m_title += dxgiAdapterDesc.VendorId == 0x1414 && dxgiAdapterDesc.DeviceId == 0x8c ? L" (WARP)" : L" (Software)";
	ThrowIfFailed(hr);

	// Create the command queue.
	N_RETURN(m_device->GetCommandQueue(m_commandQueue, CommandListType::DIRECT, CommandQueueFlag::NONE), ThrowIfFailed(E_FAIL));

	// Describe and create the swap chain.
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.BufferCount = FrameCount;
	swapChainDesc.Width = m_width;
	swapChainDesc.Height = m_height;
	swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.SampleDesc.Count = 1;

	com_ptr<IDXGISwapChain1> swapChain;
	ThrowIfFailed(factory->CreateSwapChainForHwnd(
		m_commandQueue.get(),		// Swap chain needs the queue so that it can force a flush on it.
		Win32Application::GetHwnd(),
		&swapChainDesc,
		nullptr,
		nullptr,
		&swapChain
	));

	// This sample does not support fullscreen transitions.
	ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));

	ThrowIfFailed(swapChain.As(&m_swapChain));
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	m_descriptorTableCache = DescriptorTableCache::MakeShared(m_device, L"DescriptorTableCache");

	// Create frame resources.
	// Create a RTV and a command allocator for each frame.
	for (auto n = 0u; n < FrameCount; n++)
	{
		m_renderTargets[n] = RenderTarget::MakeUnique();
		N_RETURN(m_renderTargets[n]->CreateFromSwapChain(m_device, m_swapChain, n), ThrowIfFailed(E_FAIL));
		N_RETURN(m_device->GetCommandAllocator(m_commandAllocators[n], CommandListType::DIRECT), ThrowIfFailed(E_FAIL));
	}

	// Create output views
	m_depth = DepthStencil::MakeUnique();
	m_depth->Create(m_device, m_width, m_height, Format::D24_UNORM_S8_UINT,
		ResourceFlag::DENY_SHADER_RESOURCE, 1, 1, 1, 1.0f, 0, false, L"Depth");
}

// Load the sample assets.
void VolumeRender::LoadAssets()
{
	const auto counter = RawBuffer::MakeUnique();
	N_RETURN(counter->Create(m_device, sizeof(uint32_t), ResourceFlag::DENY_SHADER_RESOURCE,
		MemoryType::READBACK, 0, nullptr, 0), ThrowIfFailed(E_FAIL));

	// Create the command list.
	m_commandList = CommandList::MakeUnique();
	const auto pCommandList = m_commandList.get();
	N_RETURN(m_device->GetCommandList(pCommandList, 0, CommandListType::DIRECT,
		m_commandAllocators[m_frameIndex], nullptr), ThrowIfFailed(E_FAIL));

	vector<Resource> uploaders(0);
	m_rayCaster = make_unique<RayCaster>(m_device);
	if (!m_rayCaster) ThrowIfFailed(E_FAIL);
	if (!m_rayCaster->Init(m_width, m_height, m_descriptorTableCache,
		Format::B8G8R8A8_UNORM, Format::D24_UNORM_S8_UINT, m_gridSize))
		ThrowIfFailed(E_FAIL);

	m_particleRenderer = make_unique<ParticleRenderer>(m_device);
	if (!m_particleRenderer) ThrowIfFailed(E_FAIL);
	if (!m_particleRenderer->Init(m_width, m_height, m_descriptorTableCache,
		Format::B8G8R8A8_UNORM, Format::D24_UNORM_S8_UINT, m_numParticles, m_particleSize))
		ThrowIfFailed(E_FAIL);

	if (m_volumeFile.empty()) m_rayCaster->InitGridData(pCommandList);
	else m_rayCaster->LoadGridData(pCommandList, m_volumeFile.c_str(), uploaders);
	m_particleRenderer->GenerateParticles(pCommandList, m_rayCaster->GetGridSRVTable(pCommandList));

	// Close the command list and execute it to begin the initial GPU setup.
	ThrowIfFailed(pCommandList->Close());
	m_commandQueue->SubmitCommandList(pCommandList);

	// Create synchronization objects and wait until assets have been uploaded to the GPU.
	{
		N_RETURN(m_device->GetFence(m_fence, m_fenceValues[m_frameIndex]++, FenceFlag::NONE), ThrowIfFailed(E_FAIL));

		// Create an event handle to use for frame synchronization.
		m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (m_fenceEvent == nullptr)
		{
			ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
		}

		// Wait for the command list to execute; we are reusing the same command 
		// list in our main loop but for now, we just want to wait for setup to 
		// complete before continuing.
		WaitForGpu();
	}

	// Projection
	const auto aspectRatio = m_width / static_cast<float>(m_height);
	const auto proj = XMMatrixPerspectiveFovLH(g_FOVAngleY, aspectRatio, g_zNear, g_zFar);
	XMStoreFloat4x4(&m_proj, proj);

	// View initialization
	m_focusPt = XMFLOAT3(0.0f, 0.0f, 0.0f);
	m_eyePt = XMFLOAT3(4.0f, 16.0f, -40.0f);
	const auto focusPt = XMLoadFloat3(&m_focusPt);
	const auto eyePt = XMLoadFloat3(&m_eyePt);
	const auto view = XMMatrixLookAtLH(eyePt, focusPt, XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f));
	XMStoreFloat4x4(&m_view, view);
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

	// View
	//const auto eyePt = XMLoadFloat3(&m_eyePt);
	const auto view = XMLoadFloat4x4(&m_view);
	const auto proj = XMLoadFloat4x4(&m_proj);
	const auto viewProj = view * proj;
	m_rayCaster->UpdateFrame(m_frameIndex, viewProj, m_eyePt);
	m_particleRenderer->UpdateFrame(view, proj, m_eyePt);
}

// Render the scene.
void VolumeRender::OnRender()
{
	// Record all the commands we need to render the scene into the command list.
	PopulateCommandList();

	// Execute the command list.
	m_commandQueue->SubmitCommandList(m_commandList.get());

	// Present the frame.
	ThrowIfFailed(m_swapChain->Present(0, 0));

	MoveToNextFrame();
}

void VolumeRender::OnDestroy()
{
	// Ensure that the GPU is no longer referencing resources that are about to be
	// cleaned up by the destructor.
	WaitForGpu();

	CloseHandle(m_fenceEvent);
}

// User hot-key interactions.
void VolumeRender::OnKeyUp(uint8_t key)
{
	switch (key)
	{
	case 0x20:	// case VK_SPACE:
		m_isPaused = !m_isPaused;
		break;
	case 0x70:	//case VK_F1:
		m_showFPS = !m_showFPS;
		break;
	case 'A':
		g_renderMethod = static_cast<RenderMethod>((g_renderMethod + NUM_RENDER_METHOD - 1) % NUM_RENDER_METHOD);
		break;
	case 'S':
		g_renderMethod = static_cast<RenderMethod>((g_renderMethod + 1) % NUM_RENDER_METHOD);
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
	wstring_convert<codecvt_utf8<wchar_t>> converter;
	DXFramework::ParseCommandLineArgs(argv, argc);

	for (auto i = 1; i < argc; ++i)
	{
		if (_wcsnicmp(argv[i], L"-gridSize", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/gridSize", wcslen(argv[i])) == 0)
		{
			m_gridSize.x = ++i < argc ? static_cast<uint32_t>(_wtof(argv[i])) : m_gridSize.x;
			m_gridSize.y = ++i < argc ? static_cast<uint32_t>(_wtof(argv[i])) : m_gridSize.y;
			m_gridSize.z = ++i < argc ? static_cast<uint32_t>(_wtof(argv[i])) : m_gridSize.z;
		}
		else if (_wcsnicmp(argv[i], L"-particles", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/particles", wcslen(argv[i])) == 0)
		{
			m_numParticles = ++i < argc ? _wtoi(argv[i]) : m_numParticles;
		}
		else if (_wcsnicmp(argv[i], L"-particleSize", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/particleSize", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"-pSize", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/pSize", wcslen(argv[i])) == 0)
		{
			m_particleSize = ++i < argc ? static_cast<float>(_wtof(argv[i])) : m_particleSize;
		}
		else if (_wcsnicmp(argv[i], L"-volume", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/volume", wcslen(argv[i])) == 0)
		{
			m_volumeFile = ++i < argc ? argv[i] : m_volumeFile;
		}
	}
}

void VolumeRender::PopulateCommandList()
{
	// Command list allocators can only be reset when the associated 
	// command lists have finished execution on the GPU; apps should use 
	// fences to determine GPU execution progress.
	ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());

	// However, when ExecuteCommandList() is called on a particular command 
	// list, that command list can then be reset at any time and must be before 
	// re-recording.
	const auto pCommandList = m_commandList.get();
	ThrowIfFailed(pCommandList->Reset(m_commandAllocators[m_frameIndex], nullptr));

	// Record commands.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	pCommandList->SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	ResourceBarrier barriers[1];
	auto numBarriers = m_renderTargets[m_frameIndex]->SetBarrier(barriers, ResourceState::RENDER_TARGET);
	pCommandList->Barrier(numBarriers, barriers);

	// Clear render target
	const float clearColor[4] = { 0.2f, 0.2f, 0.2f, 0.0f };
	pCommandList->ClearRenderTargetView(m_renderTargets[m_frameIndex]->GetRTV(), clearColor);
	pCommandList->ClearDepthStencilView(m_depth->GetDSV(), ClearFlag::DEPTH, 1.0f);

	pCommandList->OMSetRenderTargets(1, &m_renderTargets[m_frameIndex]->GetRTV(), &m_depth->GetDSV());

	// Set viewport
	Viewport viewport(0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));
	RectRange scissorRect(0, 0, m_width, m_height);
	pCommandList->RSSetViewports(1, &viewport);
	pCommandList->RSSetScissorRects(1, &scissorRect);

	switch (g_renderMethod)
	{
	case RAY_MARCH_MERGED:
		m_rayCaster->Render(pCommandList, m_frameIndex, false);
		break;
	case RAY_MARCH_SPLITTED:
		m_rayCaster->Render(pCommandList, m_frameIndex);
		break;
	case PARTICLE_OIT:
		m_rayCaster->RayMarchL(pCommandList, m_frameIndex);
		m_particleRenderer->Render(pCommandList, m_rayCaster->GetLightMap(),
			m_rayCaster->GetLightSRVTable(), m_renderTargets[m_frameIndex]->GetRTV(), m_depth->GetDSV());
		break;
	default:
		m_rayCaster->RayMarchL(pCommandList, m_frameIndex);
		m_particleRenderer->ShowParticles(pCommandList, m_rayCaster->GetLightMap(),
			m_rayCaster->GetLightSRVTable());
	}
	
	// Indicate that the back buffer will now be used to present.
	numBarriers = m_renderTargets[m_frameIndex]->SetBarrier(barriers, ResourceState::PRESENT);
	pCommandList->Barrier(numBarriers, barriers);

	ThrowIfFailed(pCommandList->Close());
}

// Wait for pending GPU work to complete.
void VolumeRender::WaitForGpu()
{
	// Schedule a Signal command in the queue.
	ThrowIfFailed(m_commandQueue->Signal(m_fence.get(), m_fenceValues[m_frameIndex]));

	// Wait until the fence has been processed.
	ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
	WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);

	// Increment the fence value for the current frame.
	m_fenceValues[m_frameIndex]++;
}

// Prepare to render the next frame.
void VolumeRender::MoveToNextFrame()
{
	// Schedule a Signal command in the queue.
	const auto currentFenceValue = m_fenceValues[m_frameIndex];
	ThrowIfFailed(m_commandQueue->Signal(m_fence.get(), currentFenceValue));

	// Update the frame index.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// If the next frame is not ready to be rendered yet, wait until it is ready.
	if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
	{
		ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
		WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
	}

	// Set the fence value for the next frame.
	m_fenceValues[m_frameIndex] = currentFenceValue + 1;
}

double VolumeRender::CalculateFrameStats(float* pTimeStep)
{
	static int frameCnt = 0;
	static double elapsedTime = 0.0;
	static double previousTime = 0.0;
	const auto totalTime = m_timer.GetTotalSeconds();
	++frameCnt;

	const auto timeStep = static_cast<float>(totalTime - elapsedTime);

	// Compute averages over one second period.
	if ((totalTime - elapsedTime) >= 1.0f)
	{
		float fps = static_cast<float>(frameCnt) / timeStep;	// Normalize to an exact second.

		frameCnt = 0;
		elapsedTime = totalTime;

		wstringstream windowText;
		windowText << L"    fps: ";
		if (m_showFPS) windowText << setprecision(2) << fixed << fps;
		else windowText << L"[F1]";

		windowText << L"    [A][S] ";
		switch (g_renderMethod)
		{
		case RAY_MARCH_MERGED:
			windowText << L"Ray marching without splitted lighting pass";
			break;
		case RAY_MARCH_SPLITTED:
			windowText << L"Ray marching with splitted lighting pass";
			break;
		case PARTICLE_OIT:
			windowText << L"Particle rendering with weighted blended OIT";
			break;
		default:
			windowText << L"Simple particle rendering";
		}

		SetCustomWindowText(windowText.str().c_str());
	}

	if (pTimeStep)* pTimeStep = static_cast<float>(totalTime - previousTime);
	previousTime = totalTime;

	return totalTime;
}
