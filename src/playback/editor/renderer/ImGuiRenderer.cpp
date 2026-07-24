#include "ImGuiRenderer.h"

#include "playback/editor/renderer/D3D12Compat.h"
#include "playback/editor/renderer/D3D12Hooks.h"
#include "playback/editor/renderer/ReplayMouseHook.h"

#include "playback/Playback.h"
#include "playback/editor/context/EditorContext.h"
#include "playback/editor/ui/ReplayUILayout.h"
#include "playback/editor/ui/ReplayView.h"


#include "imgui.h"
#include "imgui_impl_dx12.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace playback::editor::renderer {

namespace {

class ImGuiContextRestore {
public:
    ImGuiContextRestore() : mPrevious(ImGui::GetCurrentContext()) {}
    ~ImGuiContextRestore() { ImGui::SetCurrentContext(mPrevious); }

private:
    ImGuiContext* mPrevious;
};

class MouseInputAttempt {
public:
    ~MouseInputAttempt() {
        if (!mCommitted) setReplayMouseInputActive(false);
    }
    void commit() { mCommitted = true; }

private:
    bool mCommitted{};
};

uint64_t getSwapChainArea(IDXGISwapChain* swapChain) {
    ComPtr<ID3D12Resource> backBuffer;
    if (!swapChain || FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return 0;
    auto desc = backBuffer->GetDesc();
    return desc.Width * static_cast<uint64_t>(desc.Height);
}

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

constexpr auto SwapChainReplacementDelay = std::chrono::milliseconds(500);

bool waitForFence(UINT64 val, ComPtr<ID3D12Fence> const& fence, HANDLE event) {
    if (val == 0 || !fence || !event) return true;
    if (fence->GetCompletedValue() >= val) return true;
    if (FAILED(fence->SetEventOnCompletion(val, event))) return false;
    return WaitForSingleObject(event, GpuWaitTimeoutMs) == WAIT_OBJECT_0;
}

struct FrameResources {
    ComPtr<ID3D12CommandAllocator>    commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12Resource>            backBuffer;
    ComPtr<ID3D12Resource>            gameTexture;
    D3D12_CPU_DESCRIPTOR_HANDLE       rtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE       gameSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE       gameSrvGpu{};
    UINT64                            fenceValue{};
};

void allocateSrv(
    std::array<bool, SrvDescriptorCount>& used,
    ID3D12DescriptorHeap*                 heap,
    UINT                                  descSize,
    D3D12_CPU_DESCRIPTOR_HANDLE&          cpu,
    D3D12_GPU_DESCRIPTOR_HANDLE&          gpu
) {
    cpu.ptr = 0;
    gpu.ptr = 0;
    if (!heap || descSize == 0) return;
    auto it = std::ranges::find(used, false);
    if (it == used.end()) return;
    *it         = true;
    size_t idx  = static_cast<size_t>(std::distance(used.begin(), it));
    cpu         = heap->GetCPUDescriptorHandleForHeapStart();
    gpu         = heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr    += static_cast<SIZE_T>(idx) * static_cast<SIZE_T>(descSize);
    gpu.ptr    += static_cast<UINT64>(idx) * static_cast<UINT64>(descSize);
}

void freeSrv(
    std::array<bool, SrvDescriptorCount>& used,
    ID3D12DescriptorHeap*                 heap,
    UINT                                  descSize,
    D3D12_CPU_DESCRIPTOR_HANDLE           cpuHandle
) {
    if (!heap || descSize == 0 || cpuHandle.ptr == 0) return;
    SIZE_T base = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    if (cpuHandle.ptr < base) return;
    SIZE_T off = cpuHandle.ptr - base;
    if (off % descSize != 0) return;
    size_t idx = static_cast<size_t>(off / descSize);
    if (idx < used.size()) used[idx] = false;
}

} // namespace

struct ImGuiRenderer::Impl {
    std::mutex                            mutex;
    EditorContext*                        editorContext{};
    IDXGISwapChain*                       swapChain{};
    ComPtr<IDXGISwapChain3>               swapChain3;
    ComPtr<ID3D12Device>                  device;
    ComPtr<ID3D12CommandQueue>            commandQueue;
    ComPtr<ID3D12DescriptorHeap>          rtvHeap;
    ComPtr<ID3D12DescriptorHeap>          srvHeap;
    ComPtr<ID3D12Fence>                   fence;
    std::vector<FrameResources>           frames;
    std::vector<UINT64>                   frameFences;
    std::array<bool, SrvDescriptorCount>  srvUsed{};
    HANDLE                                fenceEvent{};
    ImGuiContext*                         imguiCtx{};
    DXGI_FORMAT                           rtvFormat{};
    UINT                                  rtvDescSize{};
    UINT                                  srvDescSize{};
    UINT64                                lastFenceValue{};
    size_t                                frameCursor{};
    uint64_t                              surfaceArea{};
    bool                                  unfenced{};
    bool                                  initialized{};
    bool                                  backendInit{};
    bool                                  renderingDisabled{};
    bool                                  initFailed{};
    bool                                  missingQueue{};
    std::chrono::steady_clock::time_point lastInitAttempt{};
    std::chrono::steady_clock::time_point lastFrameTime{};
    std::chrono::steady_clock::time_point lastPresent{};

    bool init(IDXGISwapChain* sc, ID3D12CommandQueue* cq) {
        ComPtr<IDXGISwapChain3> sc3;
        if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return false;
        ComPtr<ID3D12Device> dev;
        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev)))) return false;
        ComPtr<ID3D12Device> qdev;
        if (FAILED(cq->GetDevice(IID_PPV_ARGS(&qdev)))) return false;
        ComPtr<IUnknown> di, qi;
        if (FAILED(dev.As(&di)) || FAILED(qdev.As(&qi))) return false;
        if (di.Get() != qi.Get()) return false;

        DXGI_SWAP_CHAIN_DESC1 scDesc{};
        if (FAILED(sc3->GetDesc1(&scDesc)) || scDesc.BufferCount == 0) return false;
        ComPtr<ID3D12Resource> firstBuf;
        if (FAILED(sc3->GetBuffer(0, IID_PPV_ARGS(&firstBuf)))) return false;
        auto bufDesc = firstBuf->GetDesc();
        if (bufDesc.Width < 320 || bufDesc.Height < 180) return false;
        if (bufDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || bufDesc.SampleDesc.Count != 1) return false;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{bufDesc.Format};
        if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs)))
            || (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) == 0)
            return false;

        device       = std::move(dev);
        commandQueue = cq;
        swapChain3   = std::move(sc3);
        swapChain    = sc;
        rtvFormat    = bufDesc.Format;
        surfaceArea  = bufDesc.Width * static_cast<uint64_t>(bufDesc.Height);

        D3D12_DESCRIPTOR_HEAP_DESC rh{};
        rh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh.NumDescriptors = scDesc.BufferCount;
        if (FAILED(device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtvHeap)))) {
            initialized = true;
            this->shutdown();
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC sh{};
        sh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        sh.NumDescriptors = SrvDescriptorCount;
        sh.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&srvHeap)))) {
            initialized = true;
            this->shutdown();
            return false;
        }

        rtvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        srvUsed.fill(false);
        frames.resize(scDesc.BufferCount);
        frameFences.assign(scDesc.BufferCount, 0);
        auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_HEAP_PROPERTIES
        defHeap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
        auto gd  = bufDesc;
        gd.Flags = D3D12_RESOURCE_FLAG_NONE;

        for (UINT i = 0; i < scDesc.BufferCount; ++i) {
            auto& f = frames[i];
            if (i == 0) f.backBuffer = firstBuf;
            else if (FAILED(swapChain3->GetBuffer(i, IID_PPV_ARGS(&f.backBuffer)))) {
                initialized = true;
                this->shutdown();
                return false;
            }
            f.rtv = rtv;
            device->CreateRenderTargetView(f.backBuffer.Get(), nullptr, f.rtv);
            rtv.ptr += static_cast<SIZE_T>(rtvDescSize);
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.commandAllocator))
                )) {
                initialized = true;
                this->shutdown();
                return false;
            }
            if (FAILED(device->CreateCommandList(
                    0,
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    f.commandAllocator.Get(),
                    nullptr,
                    IID_PPV_ARGS(&f.commandList)
                ))) {
                initialized = true;
                this->shutdown();
                return false;
            }
            if (FAILED(f.commandList->Close())) {
                initialized = true;
                this->shutdown();
                return false;
            }
            if (FAILED(device->CreateCommittedResource(
                    &defHeap,
                    D3D12_HEAP_FLAG_NONE,
                    &gd,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    nullptr,
                    IID_PPV_ARGS(&f.gameTexture)
                ))) {
                initialized = true;
                this->shutdown();
                return false;
            }
            allocateSrv(srvUsed, srvHeap.Get(), srvDescSize, f.gameSrvCpu, f.gameSrvGpu);
            if (f.gameSrvCpu.ptr == 0 || f.gameSrvGpu.ptr == 0) {
                initialized = true;
                this->shutdown();
                return false;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Shader4ComponentMapping   = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 1, 2, 5);
            sd.Format                    = bufDesc.Format;
            sd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MostDetailedMip = 0;
            sd.Texture2D.MipLevels       = 1;
            device->CreateShaderResourceView(f.gameTexture.Get(), &sd, f.gameSrvCpu);
        }

        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            initialized = true;
            this->shutdown();
            return false;
        }
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent) {
            initialized = true;
            this->shutdown();
            return false;
        }

        IMGUI_CHECKVERSION();
        imguiCtx = ImGui::CreateContext();
        if (!imguiCtx) {
            initialized = true;
            this->shutdown();
            return false;
        }
        ImGui::SetCurrentContext(imguiCtx);
        auto& io               = ImGui::GetIO();
        io.IniFilename         = nullptr;
        io.LogFilename         = nullptr;
        io.BackendPlatformName = "playback_d3d12_overlay";
        ImGui::StyleColorsDark();
        auto& style            = ImGui::GetStyle();
        style.AntiAliasedLines = true;
        style.AntiAliasedFill  = true;

        ImGui_ImplDX12_InitInfo ii{};
        ii.Device            = device.Get();
        ii.CommandQueue      = commandQueue.Get();
        ii.NumFramesInFlight = static_cast<int>(frames.size());
        ii.RTVFormat         = rtvFormat;
        ii.DSVFormat         = DXGI_FORMAT_UNKNOWN;
        ii.UserData          = this;
        ii.SrvDescriptorHeap = srvHeap.Get();
        ii.SrvDescriptorAllocFn =
            [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
                auto& impl = *static_cast<Impl*>(info->UserData);
                allocateSrv(impl.srvUsed, impl.srvHeap.Get(), impl.srvDescSize, *cpu, *gpu);
            };
        ii.SrvDescriptorFreeFn =
            [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
                auto& impl = *static_cast<Impl*>(info->UserData);
                freeSrv(impl.srvUsed, impl.srvHeap.Get(), impl.srvDescSize, cpu);
            };
        if (!ImGui_ImplDX12_Init(&ii)) {
            initialized = true;
            this->shutdown();
            return false;
        }
        backendInit = true;
        if (!ImGui_ImplDX12_CreateDeviceObjects()) {
            initialized = true;
            this->shutdown();
            return false;
        }

        lastFrameTime = std::chrono::steady_clock::now();
        lastPresent   = lastFrameTime;
        initialized   = true;
        initFailed    = false;
        missingQueue  = false;
        getLogger().info(
            "Replay ImGui timeline initialized ({}x{}, {} buffers)",
            bufDesc.Width,
            bufDesc.Height,
            scDesc.BufferCount
        );
        return true;
    }

    void shutdown() {
        setReplayMouseInputActive(false);
        if (fence && commandQueue && unfenced) {
            UINT64 fv = lastFenceValue + 1;
            if (SUCCEEDED(commandQueue->Signal(fence.Get(), fv))) {
                lastFenceValue = fv;
                unfenced       = false;
            }
        }
        if (lastFenceValue != 0) waitForFence(lastFenceValue, fence, fenceEvent);
        if (imguiCtx) {
            auto* prev = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(imguiCtx);
            if (backendInit) ImGui_ImplDX12_Shutdown();
            ImGui::DestroyContext(imguiCtx);
            ImGui::SetCurrentContext(prev == imguiCtx ? nullptr : prev);
        }
        backendInit = false;
        imguiCtx    = nullptr;
        frames.clear();
        frameFences.clear();
        rtvHeap.Reset();
        srvHeap.Reset();
        fence.Reset();
        if (fenceEvent) CloseHandle(fenceEvent);
        fenceEvent = nullptr;
        swapChain3.Reset();
        device.Reset();
        commandQueue.Reset();
        swapChain         = nullptr;
        rtvDescSize       = 0;
        srvDescSize       = 0;
        lastFenceValue    = 0;
        frameCursor       = 0;
        surfaceArea       = 0;
        unfenced          = false;
        initialized       = false;
        renderingDisabled = false;
        missingQueue      = false;
        srvUsed.fill(false);
    }
};

ImGuiRenderer gImGuiRenderer;

ImGuiRenderer::ImGuiRenderer() : mImpl(std::make_unique<Impl>()) {}
ImGuiRenderer::~ImGuiRenderer() = default;

void ImGuiRenderer::setContext(EditorContext* context) {
    std::scoped_lock lock(mImpl->mutex);
    mImpl->editorContext = context;
    if (!context) setReplayMouseInputActive(false);
}

bool ImGuiRenderer::render(IDXGISwapChain* swapChain) {
    auto&            p = *mImpl;
    std::scoped_lock lk(p.mutex);
    if (!isTimelineRenderingEnabled()) return false;
    if (!swapChain) return false;
    if (!p.editorContext) return false;
    auto const state = p.editorContext->snapshot();
    if (!state.replayVisible) {
        setReplayMouseInputActive(false);
        if (p.initialized) p.shutdown();
        return false;
    }
    auto now = std::chrono::steady_clock::now();
    if (p.initialized && swapChain == p.swapChain) p.lastPresent = now;
    if (!state.hudVisible) {
        setReplayMouseInputActive(false);
        return false;
    }

    ComPtr<ID3D12CommandQueue> q;
    if (p.initialized && swapChain != p.swapChain) {
        q         = getSwapChainQueue(swapChain);
        auto area = getSwapChainArea(swapChain);
        if (!q || area == 0 || (!(now - p.lastPresent >= SwapChainReplacementDelay) && area <= p.surfaceArea))
            return false;
        p.shutdown();
    }

    MouseInputAttempt ia;
    if (!p.initialized) {
        if (!q) q = getSwapChainQueue(swapChain);
        if (!q) {
            if (!p.missingQueue) {
                getLogger().warn("Replay ImGui timeline cannot render: swap chain not captured at creation");
                p.missingQueue = true;
            }
            return false;
        }
        if (now - p.lastInitAttempt < std::chrono::milliseconds(500)) return false;
        p.lastInitAttempt = now;
        if (!p.init(swapChain, q.Get())) {
            if (!p.initFailed) {
                getLogger().warn("Unable to initialize replay ImGui timeline");
                p.initFailed = true;
            }
            return false;
        }
    }

    if (p.renderingDisabled || !p.swapChain3 || p.frames.empty()) return false;
    UINT fi = p.swapChain3->GetCurrentBackBufferIndex();
    if (fi >= static_cast<UINT>(p.frames.size())) return false;
    auto& f = p.frames[fi];
    if (!waitForFence(f.fenceValue, p.fence, p.fenceEvent)) return false;
    if (p.frameFences.empty()) return false;
    size_t slot = p.frameCursor % p.frameFences.size();
    if (!waitForFence(p.frameFences[slot], p.fence, p.fenceEvent)) return false;

    auto bd = f.backBuffer->GetDesc();
    if (bd.Width == 0 || bd.Height == 0) return false;

    ImGuiContextRestore cr;
    ImGui::SetCurrentContext(p.imguiCtx);
    auto& io                   = ImGui::GetIO();
    io.DisplaySize             = ImVec2(static_cast<float>(bd.Width), static_cast<float>(bd.Height));
    io.DisplayFramebufferScale = ImVec2(1, 1);
    auto layout                = ui::calculateReplayUILayout(io.DisplaySize.x, io.DisplaySize.y);
    io.FontGlobalScale         = std::max(0.85f, layout.scale);
    auto fn                    = std::chrono::steady_clock::now();
    io.DeltaTime    = std::clamp(std::chrono::duration<float>(fn - p.lastFrameTime).count(), 1.f / 240.f, 0.25f);
    p.lastFrameTime = fn;

    ImGui_ImplDX12_NewFrame();
    beginReplayMouseFrame(layout, io.DisplaySize.x, io.DisplaySize.y);
    ImGui::NewFrame();
    ImGui::GetBackgroundDrawList()->AddImage(
        ImTextureRef(static_cast<ImTextureID>(f.gameSrvGpu.ptr)),
        ImVec2(layout.gameViewportLeft, layout.gameViewportTop),
        ImVec2(layout.gameViewportRight, layout.gameViewportBottom)
    );
    std::vector<EditorAction> actions;
    ui::drawReplayView(state, layout, actions);
    endReplayMouseFrame();
    for (auto const action : actions) p.editorContext->submit(action);
    ImGui::Render();

    if (FAILED(f.commandAllocator->Reset())) return false;
    if (FAILED(f.commandList->Reset(f.commandAllocator.Get(), nullptr))) return false;

    D3D12_RESOURCE_BARRIER toCopy[2]{};
    toCopy[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy[0].Transition.pResource   = f.backBuffer.Get();
    toCopy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCopy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toCopy[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toCopy[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy[1].Transition.pResource   = f.gameTexture.Get();
    toCopy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCopy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toCopy[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    f.commandList->ResourceBarrier(2, toCopy);
    f.commandList->CopyResource(f.gameTexture.Get(), f.backBuffer.Get());

    D3D12_RESOURCE_BARRIER toRT[2]{};
    toRT[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRT[0].Transition.pResource   = f.gameTexture.Get();
    toRT[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRT[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toRT[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toRT[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRT[1].Transition.pResource   = f.backBuffer.Get();
    toRT[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRT[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toRT[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    f.commandList->ResourceBarrier(2, toRT);
    f.commandList->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
    float cc[]{0.055f, 0.055f, 0.065f, 1};
    f.commandList->ClearRenderTargetView(f.rtv, cc, 0, nullptr);
    ID3D12DescriptorHeap* dh[]{p.srvHeap.Get()};
    f.commandList->SetDescriptorHeaps(1, dh);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), f.commandList.Get());
    ++p.frameCursor;

    D3D12_RESOURCE_BARRIER toPresent{};
    toPresent.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toPresent.Transition.pResource   = f.backBuffer.Get();
    toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    f.commandList->ResourceBarrier(1, &toPresent);
    if (FAILED(f.commandList->Close())) return false;
    ID3D12CommandList* cl[]{f.commandList.Get()};
    p.commandQueue->ExecuteCommandLists(1, cl);
    p.unfenced = true;
    UINT64 fv  = p.lastFenceValue + 1;
    if (FAILED(p.commandQueue->Signal(p.fence.Get(), fv))) {
        p.renderingDisabled = true;
        getLogger().error("Replay ImGui timeline disabled: fence signal failed");
        return false;
    }
    p.lastFenceValue    = fv;
    p.unfenced          = false;
    f.fenceValue        = fv;
    p.frameFences[slot] = fv;
    ia.commit();
    return true;
}

bool ImGuiRenderer::beforeResize(IDXGISwapChain* sc) {
    std::scoped_lock lk(mImpl->mutex);
    if (sc == mImpl->swapChain) {
        mImpl->shutdown();
        mImpl->initFailed      = false;
        mImpl->lastInitAttempt = {};
    }
    return true;
}

void ImGuiRenderer::afterPresent(IDXGISwapChain* sc, long result) {
    if (result != DXGI_ERROR_DEVICE_REMOVED && result != DXGI_ERROR_DEVICE_RESET) return;
    std::scoped_lock lk(mImpl->mutex);
    if (sc == mImpl->swapChain) mImpl->shutdown();
    unbindSwapChainQueue(sc);
}

bool ImGuiRenderer::shutdown() {
    std::scoped_lock lk(mImpl->mutex);
    mImpl->shutdown();
    mImpl->initFailed      = false;
    mImpl->lastInitAttempt = {};
    return true;
}

} // namespace playback::editor::renderer
