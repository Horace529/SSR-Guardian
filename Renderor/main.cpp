// =============================================================================
//  DX12 基础渲染器框架 (Renderor)
//  单文件实现，包含：
//    1. DXGI 工厂 / GPU 适配器枚举与查询
//    2. D3D12 设备创建（含调试层）
//    3. 多重采样 (MSAA) 能力查询
//    4. 命令队列 / 命令分配器 / 命令列表
//    5. 交换链（双缓冲 FLIP_DISCARD）
//    6. RTV / DSV 描述符堆与后台缓冲
//    7. MSAA 离屏渲染目标 + Resolve 到后台缓冲
//    8. 深度模板缓冲
//    9. CPU/GPU 围栏同步
//   10. Win32 窗口消息循环
// =============================================================================

#define NOMINMAX

#include <Windows.h>          // Win32 窗口与消息
#include <d3d12.h>            // Direct3D 12
#include <dxgi1_6.h>          // DXGI（枚举适配器 / 交换链）
#include <d3dcompiler.h>      // 着色器编译（本文件暂未使用，保留供后续扩展）
#include <wrl.h>              // Microsoft::WRL::ComPtr
#include <DirectXMath.h>
#include <DirectXPackedVector.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// =============================================================================
//  全局配置常量
// =============================================================================
namespace Config
{
    constexpr UINT        kSwapChainBufferCount = 2;                                // 后台缓冲数量（双缓冲）
    constexpr UINT        kFramesInFlight       = kSwapChainBufferCount;            // CPU 领先 GPU 的最大帧数
    constexpr DXGI_FORMAT kBackBufferFormat     = DXGI_FORMAT_R8G8B8A8_UNORM;       // 后台缓冲格式
    constexpr DXGI_FORMAT kDepthStencilFormat   = DXGI_FORMAT_D32_FLOAT;            // 深度模板格式
    constexpr UINT        kWindowWidth          = 1280;                             // 默认客户区宽
    constexpr UINT        kWindowHeight         = 720;                              // 默认客户区高
    constexpr float       kClearColor[4]        = { 0.10f, 0.12f, 0.16f, 1.0f };    // 每帧清屏颜色
}

// =============================================================================
//  关键结构体定义
// =============================================================================

// GPU 适配器（显卡）查询结果
struct AdapterInfo
{
    std::wstring description;                       // 适配器名称，如 "NVIDIA GeForce RTX 4060"
    SIZE_T       dedicatedVideoMemory = 0;          // 专用显存字节数
    UINT         vendorId             = 0;          // 厂商 ID（NVIDIA=0x10DE / AMD=0x1002 / Intel=0x8086）
    UINT         deviceId             = 0;          // 设备 ID
    bool         isSoftware           = false;      // 是否为 WARP 等软件适配器
    bool         supportsD3D12        = false;      // 是否支持创建 D3D12 设备
};

// MSAA（多重采样抗锯齿）能力查询结果
struct MsaaInfo
{
    DXGI_FORMAT format         = DXGI_FORMAT_UNKNOWN; // 被查询的格式
    UINT        maxSampleCount = 1;                   // 硬件支持的最高采样数（1 表示不支持 MSAA）
    UINT        qualityLevels  = 0;                   // 该采样数下可用的质量级别数量
    bool        enabled        = false;               // 最终是否启用 MSAA
};

// 每一帧独立拥有的 CPU/GPU 同步资源
//   - allocator ：命令分配器不能被正在执行的帧复用，因此每帧一个
//   - fenceValue：该帧提交完成后 GPU 写入的围栏值，用于回收该帧资源
struct FrameResource
{
    ComPtr<ID3D12CommandAllocator> allocator;
    UINT64                         fenceValue = 0;
};

// =============================================================================
//  小工具函数
// =============================================================================

// HRESULT 检查：失败时打印并抛出异常，便于定位初始化阶段的问题
inline void ThrowIfFailed(HRESULT hr, const char* message)
{
    if (FAILED(hr))
    {
        std::cerr << "[DX12] 失败: " << message
                  << " (HRESULT = 0x" << std::hex << hr << ")" << std::dec << std::endl;
        throw std::runtime_error(message);
    }
}

// 宽字符串转 UTF-8，便于在控制台打印中文/显卡名
inline std::string ToUtf8(const std::wstring& w)
{
    if (w.empty())
        return {};

    const int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

// 构造一次资源状态转换屏障
inline D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource,
                                                D3D12_RESOURCE_STATES before,
                                                D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = after;
    return barrier;
}

// 描述符句柄按字节偏移
inline D3D12_CPU_DESCRIPTOR_HANDLE OffsetCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle, SIZE_T offset)
{
    handle.ptr += offset;
    return handle;
}

// =============================================================================
//  DX12Renderer：框架主类
// =============================================================================
class DX12Renderer
{
public:
    void Init(HINSTANCE hInstance, UINT width, UINT height);
    int  Run();
    void Shutdown();

private:
    // ---- 初始化各步骤 ----
    void CreateAppWindow(HINSTANCE hInstance, UINT width, UINT height);
    void CreateFactory();
    void QueryAdapters();                 // 枚举并查询所有 GPU 适配器
    void CreateDevice();                  // 创建 D3D12 设备
    void QueryMsaaSupport();              // 查询 MSAA 能力
    void CreateCommandObjects();          // 命令队列 / 分配器 / 列表
    void CreateSwapChain();               // 交换链
    void CreateDescriptorHeaps();         // RTV / DSV 堆
    void CreateRenderTargets();           // 后台缓冲 RTV + MSAA 目标 RTV
    void CreateDepthStencil();            // 深度模板缓冲
    void CreateFence();                   // 同步围栏
    void UpdateViewport();

    // ---- 每帧循环 ----
    void Render();
    void WaitForGpu();                    // 阻塞等待 GPU 完成所有已提交工作
    void Resize(UINT width, UINT height); // 实际执行尺寸变更

    void OnResize(UINT width, UINT height);// 来自 WM_SIZE，标记延迟处理

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    // ---- 窗口 ----
    HWND m_hwnd  = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;
    bool m_resizePending = false;
    UINT m_pendingWidth   = 0;
    UINT m_pendingHeight  = 0;
    bool m_ready = false;

    // ---- 核心对象 ----
    ComPtr<IDXGIFactory6>            m_factory;       // DXGI 工厂
    ComPtr<IDXGIAdapter1>            m_adapter;       // 选中的 GPU
    ComPtr<ID3D12Device>             m_device;        // D3D12 设备
    ComPtr<IDXGISwapChain4>          m_swapChain;     // 交换链
    ComPtr<ID3D12CommandQueue>       m_commandQueue;  // 命令队列
    ComPtr<ID3D12GraphicsCommandList> m_commandList;  // 命令列表

    // ---- 描述符堆 ----
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    UINT m_rtvDescriptorSize = 0;
    UINT m_dsvDescriptorSize = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE m_msaaRtvHandle = {}; // MSAA 目标的 RTV

    // ---- 渲染目标 ----
    std::vector<ComPtr<ID3D12Resource>> m_backBuffers; // 交换链后台缓冲
    ComPtr<ID3D12Resource>              m_msaaTarget;  // MSAA 离屏渲染目标
    ComPtr<ID3D12Resource>              m_depthStencil;// 深度模板缓冲
    D3D12_RESOURCE_STATES m_msaaState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    // ---- 同步 ----
    std::array<FrameResource, Config::kFramesInFlight> m_frameResources;
    ComPtr<ID3D12Fence> m_fence;
    UINT64              m_fenceValue = 0;
    HANDLE              m_fenceEvent = nullptr;
    UINT                m_frameIndex = 0;

    // ---- 视口 ----
    D3D12_VIEWPORT m_viewport = {};
    D3D12_RECT     m_scissor  = {};

    // ---- 查询结果 ----
    std::vector<AdapterInfo> m_adapters;
    MsaaInfo                 m_msaa;
};

// -----------------------------------------------------------------------------
//  初始化
// -----------------------------------------------------------------------------
void DX12Renderer::Init(HINSTANCE hInstance, UINT width, UINT height)
{
    CreateAppWindow(hInstance, width, height);
    CreateFactory();
    QueryAdapters();
    CreateDevice();
    QueryMsaaSupport();
    CreateCommandObjects();
    CreateSwapChain();
    CreateDescriptorHeaps();
    CreateRenderTargets();
    CreateDepthStencil();
    CreateFence();

    UpdateViewport();
    WaitForGpu(); // 确保初始化命令全部完成

    m_ready = true;
    std::cout << "初始化完成，MSAA 采样数 = " << m_msaa.maxSampleCount << std::endl;
}

void DX12Renderer::CreateAppWindow(HINSTANCE hInstance, UINT width, UINT height)
{
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &DX12Renderer::WindowProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DX12RenderorWindow";
    RegisterClassExW(&wc);

    m_width  = width;
    m_height = height;

    RECT rc = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"DX12 Renderor",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance,
        this /* 通过 lpParam 传入，WM_NCCREATE 中取回 */);

    if (!m_hwnd)
        throw std::runtime_error("CreateWindowExW 失败");

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
}

void DX12Renderer::CreateFactory()
{
#if defined(_DEBUG)
    // 调试层：捕获 API 误用、资源泄漏等问题
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
            debugController->EnableDebugLayer();
    }
#endif

    UINT flags = 0;
#if defined(_DEBUG)
    flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    ThrowIfFailed(CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_factory)), "CreateDXGIFactory2");
}

// 枚举系统上所有 GPU，记录信息，并挑选一个可用的硬件适配器
void DX12Renderer::QueryAdapters()
{
    ComPtr<IDXGIFactory6> factory6;
    m_factory.As(&factory6); // 优先使用按性能排序的枚举方式

    for (UINT index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = factory6
            ? factory6->EnumAdapterByGpuPreference(
                  index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))
            : m_factory->EnumAdapters1(index, &adapter);

        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        ThrowIfFailed(hr, "EnumAdapters");

        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);

        AdapterInfo info;
        info.description          = desc.Description;
        info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
        info.vendorId             = desc.VendorId;
        info.deviceId             = desc.DeviceId;
        info.isSoftware           = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

        // 仅探测正式硬件适配器是否能创建 D3D12 设备
        if (!info.isSoftware)
        {
            info.supportsD3D12 =
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                            __uuidof(ID3D12Device), nullptr));
        }

        std::cout << "[" << index << "] " << ToUtf8(info.description)
                  << " | 显存 " << (info.dedicatedVideoMemory / (1024 * 1024)) << " MB"
                  << " | D3D12 " << (info.supportsD3D12 ? "支持" : "不支持")
                  << (info.isSoftware ? " | 软件适配器" : "") << std::endl;

        m_adapters.push_back(info);

        // 选中第一个支持 D3D12 的非软件适配器
        if (!m_adapter && info.supportsD3D12)
            m_adapter = adapter;
    }

    if (!m_adapter)
        std::cout << "未找到支持 D3D12 的硬件适配器，将回退到默认设备。" << std::endl;
}

void DX12Renderer::CreateDevice()
{
    // 传入适配器则在其上创建设备；传 nullptr 使用系统默认
    ThrowIfFailed(
        D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)),
        "D3D12CreateDevice");

    std::cout << "Direct3D 12 设备创建成功!" << std::endl;

#if defined(_DEBUG)
    // 让调试层在遇到错误时直接中断，方便定位
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(m_device.As(&infoQueue)))
    {
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
    }
#endif
}

// 从高到低查找当前后台缓冲格式支持的最高 MSAA 采样数
void DX12Renderer::QueryMsaaSupport()
{
    m_msaa = {}; // maxSampleCount 默认 1（即不启用）
    m_msaa.format = Config::kBackBufferFormat;

    for (UINT sampleCount = 32; sampleCount >= 1; sampleCount >>= 1)
    {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels = {};
        levels.Format      = Config::kBackBufferFormat;
        levels.SampleCount = sampleCount;
        levels.Flags       = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;

        if (SUCCEEDED(m_device->CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels))) &&
            levels.NumQualityLevels > 0)
        {
            m_msaa.maxSampleCount = sampleCount;
            m_msaa.qualityLevels  = levels.NumQualityLevels;
            break;
        }
    }

    // 4x 及以上才值得开启（1x/2x 视觉提升有限）
    m_msaa.enabled = (m_msaa.maxSampleCount >= 4);
    if (!m_msaa.enabled)
        m_msaa.maxSampleCount = 1;

    std::cout << "MSAA 查询: 最高采样数 = " << m_msaa.maxSampleCount
              << ", 质量级别数 = " << m_msaa.qualityLevels
              << ", 启用 = " << (m_msaa.enabled ? "是" : "否") << std::endl;
}

void DX12Renderer::CreateCommandObjects()
{
    // 命令队列必须在创建交换链之前存在
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)),
                  "CreateCommandQueue");

    // 每帧一个命令分配器
    for (UINT i = 0; i < Config::kFramesInFlight; ++i)
    {
        ThrowIfFailed(m_device->CreateCommandAllocator(
                          D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_frameResources[i].allocator)),
                      "CreateCommandAllocator");
    }

    // 复用一个命令列表，每帧 Reset
    ThrowIfFailed(m_device->CreateCommandList(
                      0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                      m_frameResources[0].allocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList)),
                  "CreateCommandList");
    ThrowIfFailed(m_commandList->Close(), "CommandList::Close");
}

void DX12Renderer::CreateSwapChain()
{
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width            = m_width;
    desc.Height           = m_height;
    desc.Format           = Config::kBackBufferFormat;
    desc.Stereo           = FALSE;
    desc.SampleDesc.Count = 1;   // 交换链后台缓冲始终为单采样，MSAA 在离屏目标上完成
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount      = Config::kSwapChainBufferCount;
    desc.Scaling          = DXGI_SCALING_STRETCH;
    desc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode        = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags            = 0;

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(m_factory->CreateSwapChainForHwnd(
                      m_commandQueue.Get(), m_hwnd, &desc, nullptr, nullptr, &swapChain1),
                  "CreateSwapChainForHwnd");

    swapChain1.As(&m_swapChain);

    // 禁用 Alt+Enter 全屏，避免与消息循环冲突
    ThrowIfFailed(m_factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER),
                  "MakeWindowAssociation");

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void DX12Renderer::CreateDescriptorHeaps()
{
    // RTV 堆：后台缓冲数量 + 1 个给 MSAA 目标
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = Config::kSwapChainBufferCount + 1;
    rtvDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)),
                  "CreateDescriptorHeap(RTV)");
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // DSV 堆：一个深度模板视图
    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
    dsvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.NumDescriptors = 1;
    dsvDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m_dsvHeap)),
                  "CreateDescriptorHeap(DSV)");
    m_dsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
}

void DX12Renderer::CreateRenderTargets()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    // 1) 为每个交换链后台缓冲创建 RTV
    m_backBuffers.resize(Config::kSwapChainBufferCount);
    for (UINT i = 0; i < Config::kSwapChainBufferCount; ++i)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])),
                      "SwapChain::GetBuffer");
        m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtvHandle);
        rtvHandle = OffsetCpuHandle(rtvHandle, m_rtvDescriptorSize);
    }

    // 2) 最后一个描述符留给 MSAA 目标
    m_msaaRtvHandle = rtvHandle;

    if (m_msaa.enabled)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment        = 0;
        desc.Width            = m_width;
        desc.Height           = m_height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = Config::kBackBufferFormat;
        desc.SampleDesc.Count = m_msaa.maxSampleCount;
        desc.SampleDesc.Quality = m_msaa.qualityLevels - 1; // 使用最高质量级别
        desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_HEAP_PROPERTIES props = {};
        props.Type          = D3D12_HEAP_TYPE_DEFAULT;
        props.CreationNodeMask = 1;
        props.VisibleNodeMask  = 1;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = Config::kBackBufferFormat;
        for (int i = 0; i < 4; ++i)
            clearValue.Color[i] = Config::kClearColor[i];

        ThrowIfFailed(m_device->CreateCommittedResource(
                          &props, D3D12_HEAP_FLAG_NONE, &desc,
                          D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue,
                          IID_PPV_ARGS(&m_msaaTarget)),
                      "CreateCommittedResource(MSAA)");

        m_device->CreateRenderTargetView(m_msaaTarget.Get(), nullptr, m_msaaRtvHandle);
        m_msaaState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
}

void DX12Renderer::CreateDepthStencil()
{
    const bool msaaOn = m_msaa.enabled;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = m_width;
    desc.Height           = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = Config::kDepthStencilFormat;
    desc.SampleDesc.Count = msaaOn ? m_msaa.maxSampleCount : 1;
    desc.SampleDesc.Quality = msaaOn ? (m_msaa.qualityLevels - 1) : 0;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_HEAP_PROPERTIES props = {};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format               = Config::kDepthStencilFormat;
    clearValue.DepthStencil.Depth   = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    ThrowIfFailed(m_device->CreateCommittedResource(
                      &props, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
                      IID_PPV_ARGS(&m_depthStencil)),
                  "CreateCommittedResource(DepthStencil)");

    m_device->CreateDepthStencilView(
        m_depthStencil.Get(), nullptr, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void DX12Renderer::CreateFence()
{
    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)),
                  "CreateFence");
    m_fenceValue = 0;
    for (auto& frame : m_frameResources)
        frame.fenceValue = 0;

    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
        throw std::runtime_error("CreateEvent 失败");
}

void DX12Renderer::UpdateViewport()
{
    m_viewport = {};
    m_viewport.TopLeftX = 0.0f;
    m_viewport.TopLeftY = 0.0f;
    m_viewport.Width    = static_cast<FLOAT>(m_width);
    m_viewport.Height   = static_cast<FLOAT>(m_height);
    m_viewport.MinDepth = 0.0f;
    m_viewport.MaxDepth = 1.0f;

    m_scissor = { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
}

// -----------------------------------------------------------------------------
//  每帧渲染
// -----------------------------------------------------------------------------
void DX12Renderer::Render()
{
    if (!m_ready)
        return;

    if (m_resizePending)
    {
        Resize(m_pendingWidth, m_pendingHeight);
        m_resizePending = false;
        return;
    }

    if (m_width == 0 || m_height == 0)
        return;

    FrameResource& frame = m_frameResources[m_frameIndex];

    // 等待该帧资源上一次的 GPU 工作完成，然后才能安全 Reset 分配器
    if (m_fence->GetCompletedValue() < frame.fenceValue)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(frame.fenceValue, m_fenceEvent),
                      "SetEventOnCompletion");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    ThrowIfFailed(frame.allocator->Reset(), "CommandAllocator::Reset");
    ThrowIfFailed(m_commandList->Reset(frame.allocator.Get(), nullptr), "CommandList::Reset");

    // ---- 1. 选择本帧渲染目标并切换到 RENDER_TARGET ----
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    if (m_msaa.enabled)
    {
        if (m_msaaState != D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(
                m_msaaTarget.Get(), m_msaaState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_commandList->ResourceBarrier(1, &barrier);
            m_msaaState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        rtv = m_msaaRtvHandle;
    }
    else
    {
        D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(
            m_backBuffers[m_frameIndex].Get(),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_commandList->ResourceBarrier(1, &barrier);
        rtv = OffsetCpuHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
                              static_cast<SIZE_T>(m_frameIndex) * m_rtvDescriptorSize);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    m_commandList->ClearRenderTargetView(rtv, Config::kClearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(
        dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissor);

    // ------------------------------------------------------------------
    //  在此处录制绘制命令：
    //  m_commandList->SetGraphicsRootSignature(...);
    //  m_commandList->SetPipelineState(...);
    //  m_commandList->DrawIndexedInstanced(...);
    // ------------------------------------------------------------------

    // ---- 2. 输出到后台缓冲 ----
    if (m_msaa.enabled)
    {
        // MSAA 目标 -> RESOLVE_SOURCE，后台缓冲 -> RESOLVE_DEST，然后解析
        D3D12_RESOURCE_BARRIER barriers[2] = {
            TransitionBarrier(m_msaaTarget.Get(),
                              D3D12_RESOURCE_STATE_RENDER_TARGET,
                              D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
            TransitionBarrier(m_backBuffers[m_frameIndex].Get(),
                              D3D12_RESOURCE_STATE_PRESENT,
                              D3D12_RESOURCE_STATE_RESOLVE_DEST)
        };
        m_commandList->ResourceBarrier(2, barriers);
        m_commandList->ResolveSubresource(m_backBuffers[m_frameIndex].Get(), 0,
                                          m_msaaTarget.Get(), 0, Config::kBackBufferFormat);

        D3D12_RESOURCE_BARRIER toPresent = TransitionBarrier(
            m_backBuffers[m_frameIndex].Get(),
            D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_PRESENT);
        m_commandList->ResourceBarrier(1, &toPresent);

        m_msaaState = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    }
    else
    {
        D3D12_RESOURCE_BARRIER toPresent = TransitionBarrier(
            m_backBuffers[m_frameIndex].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        m_commandList->ResourceBarrier(1, &toPresent);
    }

    ThrowIfFailed(m_commandList->Close(), "CommandList::Close");

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    // 呈现：Present(1, 0) 表示开启垂直同步
    ThrowIfFailed(m_swapChain->Present(1, 0), "SwapChain::Present");

    // 用围栏标记本帧完成，供下次循环回收
    frame.fenceValue = ++m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), frame.fenceValue), "CommandQueue::Signal");

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void DX12Renderer::WaitForGpu()
{
    const UINT64 value = ++m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), value), "CommandQueue::Signal");

    if (m_fence->GetCompletedValue() < value)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(value, m_fenceEvent),
                      "SetEventOnCompletion");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void DX12Renderer::Resize(UINT width, UINT height)
{
    if (!m_device || width == 0 || height == 0)
        return;

    // 尺寸变更前必须让 GPU 停止使用旧资源
    WaitForGpu();

    for (auto& backBuffer : m_backBuffers)
        backBuffer.Reset();
    m_msaaTarget.Reset();
    m_depthStencil.Reset();
    m_msaaState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    m_width  = width;
    m_height = height;

    ThrowIfFailed(m_swapChain->ResizeBuffers(
                      Config::kSwapChainBufferCount, width, height,
                      Config::kBackBufferFormat, 0),
                  "SwapChain::ResizeBuffers");

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    UpdateViewport();
    CreateRenderTargets();
    CreateDepthStencil();
}

void DX12Renderer::OnResize(UINT width, UINT height)
{
    if (!m_device || width == 0 || height == 0)
        return;

    m_resizePending = true;
    m_pendingWidth  = width;
    m_pendingHeight = height;
}

// -----------------------------------------------------------------------------
//  消息循环
// -----------------------------------------------------------------------------
int DX12Renderer::Run()
{
    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        else
        {
            Render();
        }
    }

    return static_cast<int>(msg.wParam);
}

void DX12Renderer::Shutdown()
{
    if (!m_device)
        return;

    WaitForGpu();

    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

// -----------------------------------------------------------------------------
//  窗口过程
// -----------------------------------------------------------------------------
LRESULT CALLBACK DX12Renderer::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    DX12Renderer* app = reinterpret_cast<DX12Renderer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_NCCREATE:
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    case WM_SIZE:
        if (app && wParam != SIZE_MINIMIZED)
            app->OnResize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            DestroyWindow(hwnd);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// =============================================================================
//  程序入口
// =============================================================================
int main()
{
    if (!XMVerifyCPUSupport())
    {
        std::cerr << "DirectXMath not supported!" << std::endl;
        return -1;
    }
    std::cout << "DirectXMath supported!" << std::endl;

    DX12Renderer app;
    try
    {
        app.Init(GetModuleHandleW(nullptr), Config::kWindowWidth, Config::kWindowHeight);
        const int result = app.Run();
        app.Shutdown();
        return result;
    }
    catch (const std::exception& e)
    {
        std::cerr << "程序异常终止: " << e.what() << std::endl;
        MessageBoxA(nullptr, e.what(), "DX12 错误", MB_OK | MB_ICONERROR);
        return EXIT_FAILURE;
    }
}
