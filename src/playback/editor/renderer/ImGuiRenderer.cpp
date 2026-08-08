#include "ImGuiRenderer.h"

#include "playback/editor/renderer/D3D11FrameTapBackend.h"
#include "playback/editor/renderer/D3D12Compat.h"
#include "playback/editor/renderer/D3D12FrameTapBackend.h"
#include "playback/editor/renderer/D3D12Hooks.h"
#include "playback/editor/renderer/ReplayMouseHook.h"

#include "playback/Playback.h"
#include "playback/editor/context/EditorContext.h"
#include "playback/editor/exporting/ExportActivity.h"
#include "playback/editor/input/EditorInput.h"
#include "playback/editor/renderer/ReplayUILayout.h"
#include "playback/editor/ui/ReplayEditor.h"
#include "playback/functions/render/ReplayThumbnail.h"
#include "playback/screen/select_replay/SelectReplayScreen.h"


#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"

#include <d3d11.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
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

struct ImGuiSurfaceMetrics {
    ImVec2 displaySize;
    ImVec2 framebufferScale;
};

// Keep ImGui coordinates in window-client pixels while allowing Bedrock to use
// a larger offline backbuffer. The DX backends map those logical coordinates
// through FramebufferScale when they emit draw commands.
ImGuiSurfaceMetrics getImGuiSurfaceMetrics(IDXGISwapChain* swapChain, uint32_t width, uint32_t height) {
    ImGuiSurfaceMetrics metrics{
        ImVec2(static_cast<float>(width), static_cast<float>(height)),
        ImVec2(1.0f, 1.0f),
    };

    DXGI_SWAP_CHAIN_DESC swapChainDesc{};
    RECT                 clientRect{};
    if (!swapChain || FAILED(swapChain->GetDesc(&swapChainDesc))) {
        return metrics;
    }

    HWND window = swapChainDesc.OutputWindow;
    if (!window) {
        HWND const foreground = GetForegroundWindow();
        DWORD      processId{};
        if (foreground && GetWindowThreadProcessId(foreground, &processId) != 0 && processId == GetCurrentProcessId()) {
            window = foreground;
        }
    }
    if (!window || !GetClientRect(window, &clientRect)) return metrics;

    auto const clientWidth  = clientRect.right - clientRect.left;
    auto const clientHeight = clientRect.bottom - clientRect.top;
    if (clientWidth <= 0 || clientHeight <= 0) return metrics;

    metrics.displaySize = ImVec2(static_cast<float>(clientWidth), static_cast<float>(clientHeight));
    metrics.framebufferScale =
        ImVec2(static_cast<float>(width) / metrics.displaySize.x, static_cast<float>(height) / metrics.displaySize.y);
    return metrics;
}

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
    std::mutex                                        mutex;
    functions::render::FrameTap                       frameTap;
    D3D11FrameTapBackend                              d3d11FrameTap{frameTap};
    D3D12FrameTapBackend                              d3d12FrameTap{frameTap};
    std::mutex                                        thumbnailMutex;
    std::optional<functions::render::FrameTapSession> thumbnailSession;
    EditorContext*                                    editorContext{};
    IDXGISwapChain*                                   swapChain{};
    ComPtr<IDXGISwapChain3>                           swapChain3;
    ComPtr<ID3D12Device>                              device;
    ComPtr<ID3D12CommandQueue>                        commandQueue;
    ComPtr<ID3D12DescriptorHeap>                      rtvHeap;
    ComPtr<ID3D12DescriptorHeap>                      srvHeap;
    ComPtr<ID3D12Fence>                               fence;
    std::vector<FrameResources>                       frames;
    std::vector<UINT64>                               frameFences;
    std::array<bool, SrvDescriptorCount>              srvUsed{};
    HANDLE                                            fenceEvent{};
    ImGuiContext*                                     imguiCtx{};
    DXGI_FORMAT                                       rtvFormat{};
    UINT                                              rtvDescSize{};
    UINT                                              srvDescSize{};
    UINT64                                            lastFenceValue{};
    size_t                                            frameCursor{};
    uint64_t                                          surfaceArea{};
    bool                                              unfenced{};
    bool                                              initialized{};
    bool                                              backendInit{};
    bool                                              renderingDisabled{};
    bool                                              initFailed{};
    bool                                              missingQueue{};
    std::chrono::steady_clock::time_point             lastInitAttempt{};
    std::chrono::steady_clock::time_point             lastFrameTime{};
    std::chrono::steady_clock::time_point             lastPresent{};

    ComPtr<ID3D11Device>             d3d11Device;
    ComPtr<ID3D11DeviceContext>      d3d11Context;
    ComPtr<ID3D11Texture2D>          d3d11BackBuffer;
    ComPtr<ID3D11RenderTargetView>   d3d11Rtv;
    ComPtr<ID3D11Texture2D>          d3d11GameTexture;
    ComPtr<ID3D11ShaderResourceView> d3d11GameSrv;
    IDXGISwapChain*                  d3d11SwapChain{};
    ImGuiContext*                    d3d11ImguiCtx{};
    bool                             d3d11BackendInit{};
    bool                             d3d11Initialized{};
    bool                             d3d11FirstFrameLogged{};
    struct D3D11ThumbnailTexture {
        ComPtr<ID3D11Texture2D>          texture;
        ComPtr<ID3D11ShaderResourceView> srv;
    };
    std::unordered_map<std::string, D3D11ThumbnailTexture> d3d11ThumbnailTextures;

    struct D3D12ThumbnailTexture {
        ComPtr<ID3D12Resource>      resource;
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
    };
    std::unordered_map<std::string, D3D12ThumbnailTexture> d3d12ThumbnailTextures;
    std::uint64_t                                          browserSnapshotRevision{};
    bool                                                   browserVisible{};

    void clearBrowserThumbnailTextures() {
        if (lastFenceValue != 0) (void)waitForFence(lastFenceValue, fence, fenceEvent);
        for (auto& [_, texture] : d3d12ThumbnailTextures) {
            freeSrv(srvUsed, srvHeap.Get(), srvDescSize, texture.srvCpu);
        }
        d3d12ThumbnailTextures.clear();
        d3d11ThumbnailTextures.clear();
    }

    bool initD3D11(IDXGISwapChain* sc) {
        ComPtr<ID3D11Device> dev;
        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev)))) return false;
        ComPtr<ID3D11Texture2D> backBuffer;
        if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
        D3D11_TEXTURE2D_DESC desc{};
        backBuffer->GetDesc(&desc);
        if (desc.Width < 320 || desc.Height < 180 || desc.SampleDesc.Count != 1) return false;

        ComPtr<ID3D11DeviceContext> context;
        dev->GetImmediateContext(&context);
        if (!context || FAILED(dev->CreateRenderTargetView(backBuffer.Get(), nullptr, &d3d11Rtv))) return false;

        auto gameDesc           = desc;
        gameDesc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
        gameDesc.CPUAccessFlags = 0;
        gameDesc.MiscFlags      = 0;
        gameDesc.Usage          = D3D11_USAGE_DEFAULT;
        if (FAILED(dev->CreateTexture2D(&gameDesc, nullptr, &d3d11GameTexture))
            || FAILED(dev->CreateShaderResourceView(d3d11GameTexture.Get(), nullptr, &d3d11GameSrv))) {
            d3d11Rtv.Reset();
            d3d11GameTexture.Reset();
            return false;
        }

        IMGUI_CHECKVERSION();
        d3d11ImguiCtx = ImGui::CreateContext();
        if (!d3d11ImguiCtx) return false;
        ImGui::SetCurrentContext(d3d11ImguiCtx);
        auto& io               = ImGui::GetIO();
        io.IniFilename         = nullptr;
        io.LogFilename         = nullptr;
        io.BackendPlatformName = "playback_d3d11_overlay";
        std::array<wchar_t, MAX_PATH> windowsDirectory{};
        auto const                    windowsDirectoryLength =
            GetWindowsDirectoryW(windowsDirectory.data(), static_cast<UINT>(windowsDirectory.size()));
        std::filesystem::path fontPath;
        if (windowsDirectoryLength > 0 && windowsDirectoryLength < static_cast<UINT>(windowsDirectory.size())) {
            fontPath = std::filesystem::path(windowsDirectory.data()) / "Fonts" / "msyh.ttc";
        }
        auto const fontPathString = fontPath.string();
        ImFont*    font           = fontPathString.empty() ? nullptr
                                                           : io.Fonts->AddFontFromFileTTF(
                                                    fontPathString.c_str(),
                                                    14.0f,
                                                    nullptr,
                                                    io.Fonts->GetGlyphRangesChineseSimplifiedCommon()
                                                );
        if (font) io.FontDefault = font;
        else io.Fonts->AddFontDefault();
        ImFontConfig cfg;
        cfg.MergeMode     = true;
        cfg.PixelSnapH    = true;
        cfg.GlyphOffset.y = 1.0f;
        static const ImWchar iconRange[]{0xe000, 0xe6ff, 0};
        auto const           iconPath = Playback::getInstance().getSelf().getModDir() / "fonts" / "lucide.ttf";
        if (!io.Fonts->AddFontFromFileTTF(iconPath.string().c_str(), 14.0f, &cfg, iconRange)) {
            getLogger().warn("Unable to load replay icon font from {}", iconPath);
        }
        ImGui::StyleColorsDark();
        auto& style            = ImGui::GetStyle();
        style.AntiAliasedLines = true;
        style.AntiAliasedFill  = true;
        if (!ImGui_ImplDX11_Init(dev.Get(), context.Get())) {
            ImGui::DestroyContext(d3d11ImguiCtx);
            d3d11ImguiCtx = nullptr;
            return false;
        }

        d3d11Device           = std::move(dev);
        d3d11Context          = std::move(context);
        d3d11BackBuffer       = std::move(backBuffer);
        d3d11SwapChain        = sc;
        d3d11BackendInit      = true;
        d3d11Initialized      = true;
        d3d11FirstFrameLogged = false;
        lastFrameTime         = std::chrono::steady_clock::now();
        lastPresent           = lastFrameTime;
        initFailed            = false;
        return true;
    }

    void shutdownD3D11(
        functions::render::FrameTapError frameTapError   = functions::render::FrameTapError::BackendUnavailable,
        std::string                      frameTapMessage = "D3D11 frame capture backend was released"
    ) {
        if (d3d11Initialized) {
            d3d11FrameTap.reset(frameTapError, std::move(frameTapMessage));
        }
        if (d3d11Context) d3d11Context->ClearState();
        if (d3d11ImguiCtx) {
            auto* previous = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(d3d11ImguiCtx);
            if (d3d11BackendInit) ImGui_ImplDX11_Shutdown();
            ImGui::DestroyContext(d3d11ImguiCtx);
            ImGui::SetCurrentContext(previous == d3d11ImguiCtx ? nullptr : previous);
        }
        d3d11BackendInit = false;
        d3d11ImguiCtx    = nullptr;
        d3d11GameSrv.Reset();
        d3d11ThumbnailTextures.clear();
        d3d11GameTexture.Reset();
        d3d11Rtv.Reset();
        d3d11BackBuffer.Reset();
        d3d11Context.Reset();
        d3d11Device.Reset();
        d3d11SwapChain        = nullptr;
        d3d11Initialized      = false;
        d3d11FirstFrameLogged = false;
    }

    bool renderD3D11(
        IDXGISwapChain*               sc,
        bool                          renderUi,
        bool                          captureFrame,
        EditorState const&            state,
        std::optional<std::uint32_t> backBufferIndex = std::nullopt
    ) {
        if (d3d11Initialized && sc != d3d11SwapChain) shutdownD3D11();
        if (!d3d11Initialized && !initD3D11(sc)) return false;

        D3D11_TEXTURE2D_DESC desc{};
        d3d11BackBuffer->GetDesc(&desc);
        d3d11FrameTap.poll(d3d11Context.Get());
        ComPtr<ID3D11Texture2D> source = d3d11BackBuffer;
        if (backBufferIndex && *backBufferIndex != 0) {
            ComPtr<IDXGISwapChain3> indexedSwapChain;
            if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&indexedSwapChain)))
                || FAILED(indexedSwapChain->GetBuffer(*backBufferIndex, IID_PPV_ARGS(&source)))) {
                return false;
            }
            source->GetDesc(&desc);
        }
        d3d11Context->CopyResource(d3d11GameTexture.Get(), source.Get());
        if (captureFrame) {
            (void)d3d11FrameTap.capture(d3d11Device.Get(), d3d11Context.Get(), d3d11GameTexture.Get());
        }

        // Capture-only pass: copy the frame and grab the thumbnail, but never render
        // the ImGui overlay or clear the back buffer while the user is playing.
        if (!renderUi) return true;

        ImGuiContextRestore restore;
        ImGui::SetCurrentContext(d3d11ImguiCtx);
        auto const surfaceMetrics  = getImGuiSurfaceMetrics(sc, desc.Width, desc.Height);
        auto&      io              = ImGui::GetIO();
        io.DisplaySize             = surfaceMetrics.displaySize;
        io.DisplayFramebufferScale = surfaceMetrics.framebufferScale;
        auto layout                = ui::calculateReplayUILayout(io.DisplaySize.x, io.DisplaySize.y);
        io.FontGlobalScale         = std::max(1.0f, layout.scale);
        auto frameNow              = std::chrono::steady_clock::now();
        io.DeltaTime  = std::clamp(std::chrono::duration<float>(frameNow - lastFrameTime).count(), 1.f / 240.f, 0.25f);
        lastFrameTime = frameNow;

        ImGui_ImplDX11_NewFrame();
        input::syncFrame();
        beginReplayMouseFrame(io.DisplaySize.x, io.DisplaySize.y, state.browser.visible);
        ImGui::NewFrame();
        auto submit = [this](EditorAction action) {
            if (editorContext) editorContext->submit(std::move(action));
        };
        auto& replayBrowser = playback::screen::select_replay::SelectReplayScreen::getInstance();
        auto& replayEditor  = ui::ReplayEditor::getInstance();
        if (state.browser.visible) {
            replayBrowser.draw(state.browser, submit);
        } else if (state.editorVisible) {
            replayEditor.setGameTexture(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(d3d11GameSrv.Get())));
            replayEditor.draw(state, submit);
            auto viewport = replayEditor.viewportVideoRect();
            setReplayGameViewport(viewport.min.x, viewport.min.y, viewport.max.x, viewport.max.y);
        }
        endReplayMouseFrame();
        ImGui::Render();

        ID3D11RenderTargetView* rtv = d3d11Rtv.Get();
        d3d11Context->OMSetRenderTargets(1, &rtv, nullptr);
        if (!state.browser.visible) {
            float clearColor[]{0.055f, 0.055f, 0.065f, 1};
            d3d11Context->ClearRenderTargetView(rtv, clearColor);
        }
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        d3d11FirstFrameLogged = true;
        return true;
    }

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
            if (FAILED(
                    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.commandAllocator))
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
        std::array<wchar_t, MAX_PATH> windowsDirectory{};
        auto const                    windowsDirectoryLength =
            GetWindowsDirectoryW(windowsDirectory.data(), static_cast<UINT>(windowsDirectory.size()));
        std::filesystem::path fontPath;
        if (windowsDirectoryLength > 0 && windowsDirectoryLength < static_cast<UINT>(windowsDirectory.size())) {
            fontPath = std::filesystem::path(windowsDirectory.data()) / "Fonts" / "msyh.ttc";
        }
        auto const fontPathString = fontPath.string();
        ImFont*    font           = nullptr;
        if (!fontPathString.empty()) {
            font = io.Fonts->AddFontFromFileTTF(
                fontPathString.c_str(),
                14.0f,
                nullptr,
                io.Fonts->GetGlyphRangesChineseSimplifiedCommon()
            );
        }
        if (font) {
            io.FontDefault = font;
        } else {
            io.Fonts->AddFontDefault();
        }

        // Merge the bundled Lucide font before the DX12 backend creates its font texture.
        {
            ImFontConfig cfg;
            cfg.MergeMode     = true;
            cfg.PixelSnapH    = true;
            cfg.GlyphOffset.y = 1.0f;
            static const ImWchar iconRange[]{0xe000, 0xe6ff, 0};
            auto const           iconPath = Playback::getInstance().getSelf().getModDir() / "fonts" / "lucide.ttf";
            if (!io.Fonts->AddFontFromFileTTF(iconPath.string().c_str(), 14.0f, &cfg, iconRange)) {
                getLogger().warn("Unable to load replay icon font from {}", iconPath);
            }
        }

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
        getLogger().debug(
            "Replay ImGui timeline initialized ({}x{}, {} buffers)",
            bufDesc.Width,
            bufDesc.Height,
            scDesc.BufferCount
        );
        return true;
    }

    void shutdown(
        functions::render::FrameTapError frameTapError   = functions::render::FrameTapError::BackendUnavailable,
        std::string                      frameTapMessage = "D3D12 frame capture backend was released"
    ) {
        setReplayMouseInputActive(false);
        shutdownD3D11(frameTapError, frameTapMessage);
        if (initialized) frameTap.failActive(frameTapError, frameTapMessage);
        if (fence && commandQueue && unfenced) {
            UINT64 fv = lastFenceValue + 1;
            if (SUCCEEDED(commandQueue->Signal(fence.Get(), fv))) {
                lastFenceValue = fv;
                unfenced       = false;
            }
        }
        if (lastFenceValue != 0) waitForFence(lastFenceValue, fence, fenceEvent);
        if (initialized) d3d12FrameTap.reset(frameTapError, std::move(frameTapMessage));
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
        d3d12ThumbnailTextures.clear();
    }

    // Uploads a decoded RGBA8 thumbnail into a D3D12 SRV on the game's command queue and waits
    // for the upload to finish so the returned GPU descriptor is immediately usable by ImGui.
    void* acquireD3D12ThumbnailTexture(std::string const& key, functions::render::ReplayThumbnailPixels const& pixels) {
        if (!device || !commandQueue || !srvHeap || srvDescSize == 0 || pixels.width == 0 || pixels.height == 0) {
            return nullptr;
        }

        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width            = pixels.width;
        texDesc.Height           = pixels.height;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels        = 1;
        texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES const
            defaultHeap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
        D3D12ThumbnailTexture tex;
        if (FAILED(device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &texDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&tex.resource)
            ))) {
            return nullptr;
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64                             totalBytes{};
        device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);
        D3D12_RESOURCE_DESC const uploadDesc{
            D3D12_RESOURCE_DIMENSION_BUFFER,
            0,
            totalBytes,
            1,
            1,
            1,
            DXGI_FORMAT_UNKNOWN,
            {1, 0},
            D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            D3D12_RESOURCE_FLAG_NONE
        };
        D3D12_HEAP_PROPERTIES const
            uploadHeap{D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
        ComPtr<ID3D12Resource> upload;
        if (FAILED(device->CreateCommittedResource(
                &uploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &uploadDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&upload)
            ))) {
            return nullptr;
        }

        void* mapped{};
        if (FAILED(upload->Map(0, nullptr, &mapped))) return nullptr;
        auto const rowPitch = footprint.Footprint.RowPitch;
        auto*      dst      = static_cast<uint8_t*>(mapped);
        for (uint32_t y = 0; y < pixels.height; ++y) {
            std::memcpy(
                dst + static_cast<size_t>(y) * rowPitch,
                pixels.rgba.data() + static_cast<size_t>(y) * pixels.width * 4,
                static_cast<size_t>(pixels.width) * 4
            );
        }
        upload->Unmap(0, nullptr);

        ComPtr<ID3D12CommandAllocator>    alloc;
        ComPtr<ID3D12GraphicsCommandList> list;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))
            || FAILED(
                device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))
            )) {
            return nullptr;
        }
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource       = upload.Get();
        src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION texDst{};
        texDst.pResource        = tex.resource.Get();
        texDst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        texDst.SubresourceIndex = 0;
        list->CopyTextureRegion(&texDst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = tex.resource.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &barrier);
        if (FAILED(list->Close())) return nullptr;

        allocateSrv(srvUsed, srvHeap.Get(), srvDescSize, tex.srvCpu, tex.srvGpu);
        if (tex.srvCpu.ptr == 0 || tex.srvGpu.ptr == 0) return nullptr;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping   = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 1, 2, 3);
        srvDesc.Format                    = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels       = 1;
        device->CreateShaderResourceView(tex.resource.Get(), &srvDesc, tex.srvCpu);

        ID3D12CommandList* const lists[]{list.Get()};
        commandQueue->ExecuteCommandLists(1, lists);
        UINT64 fv = lastFenceValue + 1;
        if (FAILED(commandQueue->Signal(fence.Get(), fv)) || !waitForFence(fv, fence, fenceEvent)) {
            freeSrv(srvUsed, srvHeap.Get(), srvDescSize, tex.srvCpu);
            return nullptr;
        }
        lastFenceValue = fv;

        auto [it, inserted] = d3d12ThumbnailTextures.emplace(key, std::move(tex));
        return inserted ? reinterpret_cast<void*>(it->second.srvGpu.ptr) : nullptr;
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

void ImGuiRenderer::requestReplayThumbnailCapture() {
    std::scoped_lock lock(mImpl->thumbnailMutex);
    if (mImpl->thumbnailSession) {
        mImpl->frameTap.cancel(*mImpl->thumbnailSession);
        mImpl->frameTap.close(*mImpl->thumbnailSession);
        mImpl->thumbnailSession.reset();
    }
    functions::render::FrameTapSession session;
    auto const                         opened = mImpl->frameTap.open({1, true}, session);
    if (opened != functions::render::FrameTapOpenResult::Opened) return;
    functions::render::FrameTicket const ticket{0, 0, 1};
    if (mImpl->frameTap.tryArm(session, ticket) != functions::render::FrameTapArmResult::Armed) {
        mImpl->frameTap.close(session);
        return;
    }
    mImpl->thumbnailSession = session;
}

bool ImGuiRenderer::saveReplayThumbnail(std::filesystem::path const& output) {
    std::optional<functions::render::FrameTapSession> session;
    {
        std::scoped_lock lock(mImpl->thumbnailMutex);
        session = mImpl->thumbnailSession;
    }
    if (!session) return false;
    auto       frame = mImpl->frameTap.waitPop(*session, std::chrono::milliseconds(GpuWaitTimeoutMs));
    bool const saved = frame && functions::render::writeReplayThumbnailPng(output, *frame, 640, 360);
    mImpl->frameTap.close(*session);
    {
        std::scoped_lock lock(mImpl->thumbnailMutex);
        if (mImpl->thumbnailSession && mImpl->thumbnailSession->id == session->id) mImpl->thumbnailSession.reset();
    }
    return saved;
}

functions::render::FrameTap& ImGuiRenderer::frameTap() { return mImpl->frameTap; }

void* ImGuiRenderer::acquireReplayThumbnailTexture(std::string_view key, std::string_view png) {
    auto& p      = *mImpl;
    auto  keyStr = std::string(key);
    auto  found  = p.d3d11ThumbnailTextures.find(keyStr);
    if (found != p.d3d11ThumbnailTextures.end()) return found->second.srv.Get();
    auto found12 = p.d3d12ThumbnailTextures.find(keyStr);
    if (found12 != p.d3d12ThumbnailTextures.end()) return reinterpret_cast<void*>(found12->second.srvGpu.ptr);
    if (png.empty()) return nullptr;
    functions::render::ReplayThumbnailPixels pixels;
    if (!functions::render::decodeReplayThumbnailPng(png, pixels) || pixels.width == 0 || pixels.height == 0)
        return nullptr;
    if (p.d3d11Device) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width            = pixels.width;
        desc.Height           = pixels.height;
        desc.MipLevels        = 1;
        desc.ArraySize        = 1;
        desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage            = D3D11_USAGE_DEFAULT;
        desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem     = pixels.rgba.data();
        data.SysMemPitch = pixels.width * 4;
        Impl::D3D11ThumbnailTexture texture;
        if (FAILED(p.d3d11Device->CreateTexture2D(&desc, &data, &texture.texture))
            || FAILED(p.d3d11Device->CreateShaderResourceView(texture.texture.Get(), nullptr, &texture.srv)))
            return nullptr;
        auto [it, inserted] = p.d3d11ThumbnailTextures.emplace(keyStr, std::move(texture));
        return inserted ? it->second.srv.Get() : nullptr;
    }
    if (p.device && p.commandQueue) return p.acquireD3D12ThumbnailTexture(keyStr, pixels);
    return nullptr;
}

bool ImGuiRenderer::render(IDXGISwapChain* swapChain, bool allowFrameCapture) {
    return renderInternal(swapChain, true, allowFrameCapture);
}

bool ImGuiRenderer::captureOfflineFrame(
    IDXGISwapChain*              swapChain,
    std::optional<std::uint32_t> backBufferIndex
) {
    if (!swapChain || !mImpl->frameTap.hasArmedCapture()) return false;
    bool const rendered = renderInternal(swapChain, false, true, backBufferIndex);
    auto const tapState = mImpl->frameTap.status({});
    bool const consumed = rendered && tapState.state == functions::render::FrameTapState::Active && !tapState.armed;
    if (!consumed && !mImpl->frameTap.hasArmedCapture()) {
        getLogger().error("Offline frame capture lost its armed FrameTap ticket");
    }
    return consumed;
}

void ImGuiRenderer::pollFrameCapture() {
    std::scoped_lock lock(mImpl->mutex);
    if (mImpl->d3d11Initialized) mImpl->d3d11FrameTap.poll(mImpl->d3d11Context.Get());
}

bool ImGuiRenderer::renderInternal(
    IDXGISwapChain*              swapChain,
    bool                         allowUi,
    bool                         allowFrameCapture,
    std::optional<std::uint32_t> backBufferIndex
) {
    auto&            p = *mImpl;
    std::scoped_lock lk(p.mutex);

    if (!isTimelineRenderingEnabled()) return false;
    if (!swapChain) return false;
    bool const offlineBoundaryCapture = !allowUi && allowFrameCapture;
    if (allowUi && !p.editorContext) return false;

    auto const state       = p.editorContext ? p.editorContext->snapshot() : EditorState{};
    bool const browserOpen = allowUi && state.browser.visible;
    // Keep the editor overlay alive while Minecraft displays a native menu
    // (for example the pause screen). The game frame is drawn inside the
    // editor viewport, so the menu remains contained by the game window.
    bool const editorOpen  = allowUi && state.editorVisible;
    bool const uiActive    = browserOpen || editorOpen;
    if (allowUi) input::setUiVisible(uiActive);

    auto const browserRevision = allowUi && state.browser.snapshot ? state.browser.snapshot->revision : 0;
    if (allowUi && (p.browserVisible != browserOpen || p.browserSnapshotRevision != browserRevision)) {
        p.clearBrowserThumbnailTextures();
        p.browserVisible          = browserOpen;
        p.browserSnapshotRevision = browserRevision;
    }
    if (p.d3d11Initialized) p.d3d11FrameTap.poll(p.d3d11Context.Get());
    bool const captureActive = allowFrameCapture && p.frameTap.hasArmedCapture();
    if (!uiActive && !captureActive) {
        if (allowUi) {
            setReplayMouseInputActive(false);
            if (!exporting::isOfflineRenderActivityActive() && !p.frameTap.requiresRenderPass()
                && (p.initialized || p.d3d11Initialized)) {
                p.shutdown();
            }
        }
        return false;
    }
    auto now = std::chrono::steady_clock::now();
    if (allowUi && p.initialized && swapChain == p.swapChain) p.lastPresent = now;
    if (!uiActive) {
        setReplayMouseInputActive(false);
        if (!captureActive) return false;
    }

    ComPtr<ID3D11Device> d3d11Device;
    if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&d3d11Device)))) {
        if (p.initialized) p.shutdown();
        if (uiActive) {
            MouseInputAttempt inputAttempt;
            if (!p.renderD3D11(swapChain, true, allowFrameCapture, state, backBufferIndex)) return false;
            inputAttempt.commit();
        } else if (!p.renderD3D11(swapChain, false, allowFrameCapture, state, backBufferIndex)) {
            return false;
        }
        return true;
    }

    ComPtr<ID3D12Device> d3d12Device;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&d3d12Device)))) return false;
    if (p.d3d11Initialized) p.shutdownD3D11();

    ComPtr<ID3D12CommandQueue> q;
    if (p.initialized && swapChain != p.swapChain) {
        q         = getSwapChainQueue(swapChain);
        auto area = getSwapChainArea(swapChain);
        bool const replacementDelayElapsed = now - p.lastPresent >= SwapChainReplacementDelay;
        if (!q || area == 0 || (!offlineBoundaryCapture && !replacementDelayElapsed && area <= p.surfaceArea))
            return false;
        p.shutdown();
    }

    MouseInputAttempt ia;
    if (!p.initialized) {
        if (!q) q = getSwapChainQueue(swapChain);
        // Safe fallback for an already-created swap chain: use the queue only if the
        // capture hook observed exactly one Direct queue for this device.
        if (!q) {
            ComPtr<ID3D12Device> device;
            if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device)))) q = getDeviceQueue(device.Get());
            if (q) {
                bindSwapChainQueue(swapChain, q.Get());
            }
        }
        if (!q) {
            if (!p.missingQueue) {
                getLogger().warn("Replay ImGui timeline cannot render: no command queue available");
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
    UINT fi = backBufferIndex ? *backBufferIndex : p.swapChain3->GetCurrentBackBufferIndex();
    if (fi >= static_cast<UINT>(p.frames.size())) return false;
    auto& f = p.frames[fi];
    if (!waitForFence(f.fenceValue, p.fence, p.fenceEvent)) return false;
    if (p.frameFences.empty()) return false;
    size_t slot = p.frameCursor % p.frameFences.size();
    if (!waitForFence(p.frameFences[slot], p.fence, p.fenceEvent)) return false;

    auto bd = f.backBuffer->GetDesc();
    if (bd.Width == 0 || bd.Height == 0) return false;

    ImGuiContextRestore cr;
    if (uiActive) {
        ImGui::SetCurrentContext(p.imguiCtx);
        auto const surfaceMetrics  = getImGuiSurfaceMetrics(swapChain, static_cast<uint32_t>(bd.Width), bd.Height);
        auto&      io              = ImGui::GetIO();
        io.DisplaySize             = surfaceMetrics.displaySize;
        io.DisplayFramebufferScale = surfaceMetrics.framebufferScale;
        auto layout                = ui::calculateReplayUILayout(io.DisplaySize.x, io.DisplaySize.y);
        io.FontGlobalScale         = std::max(1.0f, layout.scale);
        auto fn                    = std::chrono::steady_clock::now();
        io.DeltaTime    = std::clamp(std::chrono::duration<float>(fn - p.lastFrameTime).count(), 1.f / 240.f, 0.25f);
        p.lastFrameTime = fn;

        ImGui_ImplDX12_NewFrame();
        // Forward MCBE key events to ImGui keyboard state
        input::syncFrame();
        beginReplayMouseFrame(io.DisplaySize.x, io.DisplaySize.y, state.browser.visible);
        ImGui::NewFrame();
        auto submit = [&p](EditorAction action) {
            if (p.editorContext) p.editorContext->submit(std::move(action));
        };
        auto& replayBrowser = playback::screen::select_replay::SelectReplayScreen::getInstance();
        auto& replayEditor  = ui::ReplayEditor::getInstance();
        if (state.browser.visible) {
            replayBrowser.draw(state.browser, submit);
        } else if (state.editorVisible) {
            replayEditor.setGameTexture(static_cast<ImTextureID>(f.gameSrvGpu.ptr));
            replayEditor.draw(state, submit);
            auto viewport = replayEditor.viewportVideoRect();
            setReplayGameViewport(viewport.min.x, viewport.min.y, viewport.max.x, viewport.max.y);
        }
        endReplayMouseFrame();
        ImGui::Render();
    }

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
    toRT[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toRT[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRT[1].Transition.pResource   = f.backBuffer.Get();
    toRT[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRT[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toRT[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    f.commandList->ResourceBarrier(2, toRT);
    bool const frameTapSubmitted =
        allowFrameCapture && p.d3d12FrameTap.capture(p.device.Get(), f.commandList.Get(), f.gameTexture.Get());

    D3D12_RESOURCE_BARRIER gameTextureToSrv{};
    gameTextureToSrv.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    gameTextureToSrv.Transition.pResource   = f.gameTexture.Get();
    gameTextureToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    gameTextureToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    gameTextureToSrv.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    f.commandList->ResourceBarrier(1, &gameTextureToSrv);
    if (uiActive) {
        f.commandList->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
        if (!browserOpen) {
            float cc[]{0.055f, 0.055f, 0.065f, 1};
            f.commandList->ClearRenderTargetView(f.rtv, cc, 0, nullptr);
        }
        ID3D12DescriptorHeap* dh[]{p.srvHeap.Get()};
        f.commandList->SetDescriptorHeaps(1, dh);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), f.commandList.Get());
    }
    ++p.frameCursor;

    D3D12_RESOURCE_BARRIER toPresent{};
    toPresent.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toPresent.Transition.pResource   = f.backBuffer.Get();
    toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    f.commandList->ResourceBarrier(1, &toPresent);
    if (FAILED(f.commandList->Close())) {
        if (frameTapSubmitted) {
            p.d3d12FrameTap.submissionFailed(
                functions::render::FrameTapError::BackendUnavailable,
                "Unable to close the D3D12 frame capture command list"
            );
        }
        return false;
    }
    ID3D12CommandList* cl[]{f.commandList.Get()};
    p.commandQueue->ExecuteCommandLists(1, cl);
    p.unfenced = true;
    UINT64 fv  = p.lastFenceValue + 1;
    if (FAILED(p.commandQueue->Signal(p.fence.Get(), fv))) {
        if (frameTapSubmitted) {
            p.d3d12FrameTap.submissionFailed(
                functions::render::FrameTapError::FenceFailed,
                "Unable to signal the D3D12 frame capture fence"
            );
        }
        p.renderingDisabled = true;
        getLogger().error("Replay ImGui timeline disabled: fence signal failed");
        return false;
    }
    p.lastFenceValue    = fv;
    p.unfenced          = false;
    f.fenceValue        = fv;
    p.frameFences[slot] = fv;
    if (frameTapSubmitted) p.d3d12FrameTap.submitted(p.fence.Get(), fv);
    ia.commit();
    return true;
}

bool ImGuiRenderer::ownsSwapChain(IDXGISwapChain* swapChain) const {
    std::scoped_lock lock(mImpl->mutex);
    return swapChain && (swapChain == mImpl->swapChain || swapChain == mImpl->d3d11SwapChain);
}

bool ImGuiRenderer::beforeResize(IDXGISwapChain* sc) {
    std::scoped_lock lk(mImpl->mutex);
    if (sc == mImpl->swapChain || sc == mImpl->d3d11SwapChain) {
        mImpl->shutdown(functions::render::FrameTapError::Resize, "Swap chain resized during frame capture");
        mImpl->initFailed      = false;
        mImpl->lastInitAttempt = {};
    }
    return true;
}

void ImGuiRenderer::afterPresent(IDXGISwapChain* sc, long result) {
    if (result != DXGI_ERROR_DEVICE_REMOVED && result != DXGI_ERROR_DEVICE_RESET) return;
    std::scoped_lock lk(mImpl->mutex);
    if (sc == mImpl->swapChain || sc == mImpl->d3d11SwapChain) {
        mImpl->shutdown(functions::render::FrameTapError::DeviceLost, "Graphics device was lost during frame capture");
    }
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
