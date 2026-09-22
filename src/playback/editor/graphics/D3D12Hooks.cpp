#include "D3D12Hooks.h"

#include "playback/Playback.h"
#include "playback/editor/graphics/CameraRenderHooks.h"
#include "playback/editor/graphics/GraphicsSwitchTrace.h"
#include "playback/editor/graphics/ImGuiRenderer.h"
#include "playback/exporting/ExportActivity.h"
#include "playback/exporting/OfflineRenderClockHooks.h"
#include "playback/exporting/OfflineRenderTrace.h"
#include "playback/exporting/RenderDiagnostics.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/gui/screens/models/MinecraftScreenModel.h"
#include "mc/external/bgfx/Frame.h"
#include "mc/external/bgfx/RenderDraw.h"
#include "mc/external/bgfx/RendererContextD3D11.h"
#include "mc/external/bgfx/RendererContextD3D12.h"


#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <utility>

namespace playback::editor::graphics {

using playback::exporting::OfflineRenderTraceEvent;
using playback::exporting::OfflineRenderTraceScope;

namespace {

using PresentFn       = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn      = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, DXGI_PRESENT_PARAMETERS const*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Fn =
    HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, UINT const*, IUnknown* const*);
using CreateSwapChainFn =
    HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*,
    IUnknown*,
    HWND,
    DXGI_SWAP_CHAIN_DESC1 const*,
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC const*,
    IDXGIOutput*,
    IDXGISwapChain1**
);
using CreateSwapChainForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*,
    IUnknown*,
    IUnknown*,
    DXGI_SWAP_CHAIN_DESC1 const*,
    IDXGIOutput*,
    IDXGISwapChain1**
);
using CreateSwapChainForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*,
    IUnknown*,
    DXGI_SWAP_CHAIN_DESC1 const*,
    IDXGIOutput*,
    IDXGISwapChain1**
);

constexpr size_t SwapChainPresentIndex                     = 8;
constexpr size_t SwapChainPresent1Index                    = 22;
constexpr size_t SwapChainResizeBuffersIndex               = 13;
constexpr size_t SwapChainResizeBuffers1Index              = 39;
constexpr size_t FactoryCreateSwapChainIndex               = 10;
constexpr size_t FactoryCreateSwapChainForHwndIndex        = 15;
constexpr size_t FactoryCreateSwapChainForCoreWindowIndex  = 16;
constexpr size_t FactoryCreateSwapChainForCompositionIndex = 24;
constexpr DWORD  DetourWaitTimeoutMsLocal                  = 2000;

ll::memory::FuncPtr gOriginalPresent{};
ll::memory::FuncPtr gOriginalPresent1{};
ll::memory::FuncPtr gOriginalResizeBuffers{};
ll::memory::FuncPtr gOriginalResizeBuffers1{};
ll::memory::FuncPtr gOriginalCreateSwapChain{};
ll::memory::FuncPtr gOriginalCreateSwapChainForHwnd{};
ll::memory::FuncPtr gOriginalCreateSwapChainForCoreWindow{};
ll::memory::FuncPtr gOriginalCreateSwapChainForComposition{};

std::atomic<bool> gTimelineHooksStopping{true};
std::atomic<bool> gRendererInitHookStopping{true};
std::atomic<bool> gD3D12RendererActive{false};

std::atomic<uint32_t>   gActiveDetours{};
std::mutex              gActiveDetoursMutex;
std::condition_variable gActiveDetoursChanged;

std::atomic<uint32_t>   gActiveRendererInitDetours{};
std::mutex              gActiveRendererInitDetoursMutex;
std::condition_variable gActiveRendererInitDetoursChanged;
std::mutex              gRendererInitHookMutex;

std::recursive_mutex gTimelineHookMutex;

class ActiveDetour {
public:
    ActiveDetour() { gActiveDetours.fetch_add(1, std::memory_order_acq_rel); }
    ~ActiveDetour() {
        if (gActiveDetours.fetch_sub(1, std::memory_order_acq_rel) == 1) gActiveDetoursChanged.notify_all();
    }
};

class ActiveRendererInitDetour {
public:
    ActiveRendererInitDetour() { gActiveRendererInitDetours.fetch_add(1, std::memory_order_acq_rel); }
    ~ActiveRendererInitDetour() {
        if (gActiveRendererInitDetours.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            gActiveRendererInitDetoursChanged.notify_all();
        }
    }
};

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

template <class Callback>
void runDetourInstrumentation(Callback&& callback) noexcept {
    try {
        std::forward<Callback>(callback)();
    } catch (...) {}
}


bool renderPresentFrame(IDXGISwapChain* swapChain);

template <class T>
void* getVtableEntry(T* object, size_t index) {
    return (*reinterpret_cast<void***>(object))[index];
}

struct HookTargets {
    void* present{};
    void* present1{};
    void* resizeBuffers{};
    void* resizeBuffers1{};
    void* createSwapChain{};
    void* createSwapChainForHwnd{};
    void* createSwapChainForCoreWindow{};
    void* createSwapChainForComposition{};
};

struct HookState {
    HookTargets targets;
    bool        present{};
    bool        present1{};
    bool        resizeBuffers{};
    bool        resizeBuffers1{};
    bool        createSwapChain{};
    bool        createSwapChainForHwnd{};
    bool        createSwapChainForCoreWindow{};
    bool        createSwapChainForComposition{};
};

HookState& hookState() {
    static HookState state;
    return state;
}

bool coreInstalled(HookState const& state) {
    return state.present && state.present1 && state.resizeBuffers && state.resizeBuffers1;
}

bool captureInstalled(HookState const& state) {
    return state.createSwapChain && state.createSwapChainForHwnd && state.createSwapChainForCoreWindow
        && state.createSwapChainForComposition;
}

bool noneInstalled(HookState const& state) {
    return !state.present && !state.present1 && !state.resizeBuffers && !state.resizeBuffers1 && !state.createSwapChain
        && !state.createSwapChainForHwnd && !state.createSwapChainForCoreWindow && !state.createSwapChainForComposition;
}

#define DECLARE_DETOUR_FN(NAME, RET, ...)                                                                              \
    RET WINAPI NAME##Detour(__VA_ARGS__);                                                                              \
    RET WINAPI NAME##Detour(__VA_ARGS__)

DECLARE_DETOUR_FN(present, HRESULT, IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    ActiveDetour            activeDetour;
    OfflineRenderTraceScope trace(
        OfflineRenderTraceEvent::PresentEnter,
        OfflineRenderTraceEvent::PresentExit,
        swapChain,
        nullptr,
        syncInterval,
        flags
    );
    UINT effectiveSyncInterval = syncInterval;
    runDetourInstrumentation([&] {
        if ((flags & DXGI_PRESENT_TEST) == 0 && !gTimelineHooksStopping.load(std::memory_order_acquire)) {
            (void)renderPresentFrame(swapChain);
        }
        bool const offlinePresent =
            exporting::isOfflineRenderActivityActive() && gImGuiRenderer.ownsSwapChain(swapChain);
        effectiveSyncInterval = offlinePresent ? 0 : syncInterval;
    });
    HRESULT const result = reinterpret_cast<PresentFn>(gOriginalPresent)(swapChain, effectiveSyncInterval, flags);
    runDetourInstrumentation([&] { gImGuiRenderer.afterPresent(swapChain, result); });
    trace.result(static_cast<uint64_t>(static_cast<int64_t>(result)));
    return result;
}

DECLARE_DETOUR_FN(
    present1,
    HRESULT,
    IDXGISwapChain1*               swapChain,
    UINT                           syncInterval,
    UINT                           flags,
    DXGI_PRESENT_PARAMETERS const* parameters
) {
    ActiveDetour            activeDetour;
    OfflineRenderTraceScope trace(
        OfflineRenderTraceEvent::Present1Enter,
        OfflineRenderTraceEvent::Present1Exit,
        swapChain,
        nullptr,
        syncInterval,
        flags
    );
    auto* const baseSwapChain         = static_cast<IDXGISwapChain*>(swapChain);
    UINT        effectiveSyncInterval = syncInterval;

    DXGI_PRESENT_PARAMETERS fullSurfacePresent{};
    auto const*             effectiveParameters = parameters;
    runDetourInstrumentation([&] {
        bool timelineRendered{};
        if ((flags & DXGI_PRESENT_TEST) == 0 && !gTimelineHooksStopping.load(std::memory_order_acquire)) {
            timelineRendered = renderPresentFrame(baseSwapChain);
        }
        if (timelineRendered && parameters) {
            fullSurfacePresent                 = *parameters;
            fullSurfacePresent.DirtyRectsCount = 0;
            fullSurfacePresent.pDirtyRects     = nullptr;
            fullSurfacePresent.pScrollRect     = nullptr;
            fullSurfacePresent.pScrollOffset   = nullptr;
            effectiveParameters                = &fullSurfacePresent;
        }
        bool const offlinePresent =
            exporting::isOfflineRenderActivityActive() && gImGuiRenderer.ownsSwapChain(baseSwapChain);
        effectiveSyncInterval = offlinePresent ? 0 : syncInterval;
    });
    HRESULT const result =
        reinterpret_cast<Present1Fn>(gOriginalPresent1)(swapChain, effectiveSyncInterval, flags, effectiveParameters);
    runDetourInstrumentation([&] { gImGuiRenderer.afterPresent(swapChain, result); });
    trace.result(static_cast<uint64_t>(static_cast<int64_t>(result)));
    return result;
}

DECLARE_DETOUR_FN(
    resizeBuffers,
    HRESULT,
    IDXGISwapChain* swapChain,
    UINT            bufferCount,
    UINT            width,
    UINT            height,
    DXGI_FORMAT     format,
    UINT            flags
) {
    ActiveDetour activeDetour;
    bool         allowResize{true};
    runDetourInstrumentation([&] { allowResize = gImGuiRenderer.beforeResize(swapChain); });
    if (!allowResize) {
        runDetourInstrumentation([&] { getLogger().error("ResizeBuffers blocked: overlay GPU work did not drain"); });
        return DXGI_ERROR_INVALID_CALL;
    }
    HRESULT const result =
        reinterpret_cast<ResizeBuffersFn>(gOriginalResizeBuffers)(swapChain, bufferCount, width, height, format, flags);
    if (FAILED(result)) {
        runDetourInstrumentation([&] {
            getLogger().error(
                "ResizeBuffers failed: HRESULT=0x{:08X}, size={}x{}",
                static_cast<uint32_t>(result),
                width,
                height
            );
        });
    }
    return result;
}

DECLARE_DETOUR_FN(
    resizeBuffers1,
    HRESULT,
    IDXGISwapChain3* swapChain,
    UINT             bufferCount,
    UINT             width,
    UINT             height,
    DXGI_FORMAT      format,
    UINT             flags,
    UINT const*      creationNodeMask,
    IUnknown* const* presentQueue
) {
    ActiveDetour               activeDetour;
    ComPtr<ID3D12CommandQueue> queue;
    bool const                 updatesQueueBinding = bufferCount > 0 && presentQueue;
    bool                       allowResize{true};
    runDetourInstrumentation([&] {
        queue       = getResizePresentQueue(bufferCount, presentQueue);
        allowResize = gImGuiRenderer.beforeResize(swapChain);
    });
    if (!allowResize) {
        runDetourInstrumentation([&] { getLogger().error("ResizeBuffers1 blocked: overlay GPU work did not drain"); });
        return DXGI_ERROR_INVALID_CALL;
    }
    HRESULT const result = reinterpret_cast<ResizeBuffers1Fn>(gOriginalResizeBuffers1)(
        swapChain,
        bufferCount,
        width,
        height,
        format,
        flags,
        creationNodeMask,
        presentQueue
    );
    if (SUCCEEDED(result) && updatesQueueBinding) {
        runDetourInstrumentation([&] {
            if (queue) bindSwapChainQueue(swapChain, queue.Get());
            else unbindSwapChainQueue(swapChain);
        });
    }
    if (FAILED(result)) {
        runDetourInstrumentation([&] {
            getLogger().error(
                "ResizeBuffers1 failed: HRESULT=0x{:08X}, size={}x{}",
                static_cast<uint32_t>(result),
                width,
                height
            );
        });
    }
    return result;
}

DECLARE_DETOUR_FN(
    createSwapChain,
    HRESULT,
    IDXGIFactory*         factory,
    IUnknown*             device,
    DXGI_SWAP_CHAIN_DESC* description,
    IDXGISwapChain**      swapChain
) {
    ActiveDetour  activeDetour;
    HRESULT const result =
        reinterpret_cast<CreateSwapChainFn>(gOriginalCreateSwapChain)(factory, device, description, swapChain);
    if (SUCCEEDED(result) && swapChain && *swapChain) {
        runDetourInstrumentation([&] { bindSwapChainQueue(*swapChain, device); });
    }
    return result;
}

DECLARE_DETOUR_FN(
    createSwapChainForHwnd,
    HRESULT,
    IDXGIFactory2*                         factory,
    IUnknown*                              device,
    HWND                                   window,
    DXGI_SWAP_CHAIN_DESC1 const*           description,
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC const* fullscreenDescription,
    IDXGIOutput*                           restrictToOutput,
    IDXGISwapChain1**                      swapChain
) {
    ActiveDetour  activeDetour;
    HRESULT const result = reinterpret_cast<CreateSwapChainForHwndFn>(gOriginalCreateSwapChainForHwnd)(
        factory,
        device,
        window,
        description,
        fullscreenDescription,
        restrictToOutput,
        swapChain
    );
    if (SUCCEEDED(result) && swapChain && *swapChain) {
        runDetourInstrumentation([&] { bindSwapChainQueue(*swapChain, device); });
    }
    return result;
}

DECLARE_DETOUR_FN(
    createSwapChainForCoreWindow,
    HRESULT,
    IDXGIFactory2*               factory,
    IUnknown*                    device,
    IUnknown*                    window,
    DXGI_SWAP_CHAIN_DESC1 const* description,
    IDXGIOutput*                 restrictToOutput,
    IDXGISwapChain1**            swapChain
) {
    ActiveDetour  activeDetour;
    HRESULT const result = reinterpret_cast<CreateSwapChainForCoreWindowFn>(gOriginalCreateSwapChainForCoreWindow)(
        factory,
        device,
        window,
        description,
        restrictToOutput,
        swapChain
    );
    if (SUCCEEDED(result) && swapChain && *swapChain) {
        runDetourInstrumentation([&] { bindSwapChainQueue(*swapChain, device); });
    }
    return result;
}

DECLARE_DETOUR_FN(
    createSwapChainForComposition,
    HRESULT,
    IDXGIFactory2*               factory,
    IUnknown*                    device,
    DXGI_SWAP_CHAIN_DESC1 const* description,
    IDXGIOutput*                 restrictToOutput,
    IDXGISwapChain1**            swapChain
) {
    ActiveDetour  activeDetour;
    HRESULT const result = reinterpret_cast<CreateSwapChainForCompositionFn>(gOriginalCreateSwapChainForComposition)(
        factory,
        device,
        description,
        restrictToOutput,
        swapChain
    );
    if (SUCCEEDED(result) && swapChain && *swapChain) {
        runDetourInstrumentation([&] { bindSwapChainQueue(*swapChain, device); });
    }
    return result;
}

#undef DECLARE_DETOUR_FN

bool installPresentHook(HookState& state) {
    if (state.present) return true;
    if (ll::memory::hook(
            state.targets.present,
            ll::memory::toFuncPtr(&presentDetour),
            &gOriginalPresent,
            ll::memory::HookPriority::Normal
        )
        != 0)
        return false;
    state.present = true;
    return true;
}

bool installPresent1Hook(HookState& state) {
    if (state.present1) return true;
    if (ll::memory::hook(
            state.targets.present1,
            ll::memory::toFuncPtr(&present1Detour),
            &gOriginalPresent1,
            ll::memory::HookPriority::Normal
        )
        != 0)
        return false;
    state.present1 = true;
    return true;
}

bool installResizeBuffersHook(HookState& state) {
    if (state.resizeBuffers) return true;
    if (ll::memory::hook(
            state.targets.resizeBuffers,
            ll::memory::toFuncPtr(&resizeBuffersDetour),
            &gOriginalResizeBuffers,
            ll::memory::HookPriority::Normal
        )
        != 0)
        return false;
    state.resizeBuffers = true;
    return true;
}

bool installResizeBuffers1Hook(HookState& state) {
    if (state.resizeBuffers1) return true;
    if (ll::memory::hook(
            state.targets.resizeBuffers1,
            ll::memory::toFuncPtr(&resizeBuffers1Detour),
            &gOriginalResizeBuffers1,
            ll::memory::HookPriority::Normal
        )
        != 0)
        return false;
    state.resizeBuffers1 = true;
    return true;
}

bool installCreateSwapChainHook(HookState& state) {
    if (state.createSwapChain) return true;
    if (ll::memory::hook(
            state.targets.createSwapChain,
            ll::memory::toFuncPtr(&createSwapChainDetour),
            &gOriginalCreateSwapChain,
            ll::memory::HookPriority::Normal
        )
        != 0)
        return false;
    state.createSwapChain = true;
    return true;
}

bool installCreateSwapChainForHwndHook(HookState& state) {
    if (state.createSwapChainForHwnd) return true;
    if (ll::memory::hook(
            state.targets.createSwapChainForHwnd,
            ll::memory::toFuncPtr(&createSwapChainForHwndDetour),
            &gOriginalCreateSwapChainForHwnd,
            ll::memory::HookPriority::Normal
        )
        != 0)
        return false;
    state.createSwapChainForHwnd = true;
    return true;
}

bool installCreateSwapChainForCoreWindowHook(HookState& state) {
    if (state.createSwapChainForCoreWindow) return true;
    if (ll::memory::hook(
            state.targets.createSwapChainForCoreWindow,
            ll::memory::toFuncPtr(&createSwapChainForCoreWindowDetour),
            &gOriginalCreateSwapChainForCoreWindow,
            ll::memory::HookPriority::Normal
        )
        != 0)
        return false;
    state.createSwapChainForCoreWindow = true;
    return true;
}

bool installCreateSwapChainForCompositionHook(HookState& state) {
    if (state.createSwapChainForComposition) return true;
    if (ll::memory::hook(
            state.targets.createSwapChainForComposition,
            ll::memory::toFuncPtr(&createSwapChainForCompositionDetour),
            &gOriginalCreateSwapChainForComposition,
            ll::memory::HookPriority::Normal
        )
        != 0)
        return false;
    state.createSwapChainForComposition = true;
    return true;
}

bool installCoreHooks(HookState& state) {
    return installResizeBuffersHook(state) && installResizeBuffers1Hook(state) && installPresentHook(state)
        && installPresent1Hook(state);
}

bool installCaptureHooks(HookState& state) {
    bool ok = true;
    ok      = installCreateSwapChainHook(state) && ok;
    ok      = installCreateSwapChainForHwndHook(state) && ok;
    ok      = installCreateSwapChainForCoreWindowHook(state) && ok;
    ok      = installCreateSwapChainForCompositionHook(state) && ok;
    return ok;
}

bool removeAll(HookState& state) {
    if (state.present1 && ll::memory::unhook(state.targets.present1, ll::memory::toFuncPtr(&present1Detour)))
        state.present1 = false;
    if (state.present && ll::memory::unhook(state.targets.present, ll::memory::toFuncPtr(&presentDetour)))
        state.present = false;
    if (state.resizeBuffers1
        && ll::memory::unhook(state.targets.resizeBuffers1, ll::memory::toFuncPtr(&resizeBuffers1Detour)))
        state.resizeBuffers1 = false;
    if (state.resizeBuffers
        && ll::memory::unhook(state.targets.resizeBuffers, ll::memory::toFuncPtr(&resizeBuffersDetour)))
        state.resizeBuffers = false;
    if (state.createSwapChainForComposition
        && ll::memory::unhook(
            state.targets.createSwapChainForComposition,
            ll::memory::toFuncPtr(&createSwapChainForCompositionDetour)
        ))
        state.createSwapChainForComposition = false;
    if (state.createSwapChainForCoreWindow
        && ll::memory::unhook(
            state.targets.createSwapChainForCoreWindow,
            ll::memory::toFuncPtr(&createSwapChainForCoreWindowDetour)
        ))
        state.createSwapChainForCoreWindow = false;
    if (state.createSwapChainForHwnd
        && ll::memory::unhook(
            state.targets.createSwapChainForHwnd,
            ll::memory::toFuncPtr(&createSwapChainForHwndDetour)
        ))
        state.createSwapChainForHwnd = false;
    if (state.createSwapChain
        && ll::memory::unhook(state.targets.createSwapChain, ll::memory::toFuncPtr(&createSwapChainDetour)))
        state.createSwapChain = false;
    return noneInstalled(state);
}

bool renderPresentFrame(IDXGISwapChain* swapChain) {
    if (!swapChain) return false;
    if (!exporting::isOfflineRenderActivityActive()) return gImGuiRenderer.render(swapChain);
    if (!gImGuiRenderer.ownsSwapChain(swapChain)) return false;
    // Captures before drawing the overlay, so the export frame holds the bare world.
    return gImGuiRenderer.renderExportOverlay(swapChain);
}

// m_renderItem is a 128-byte union slot and RenderDraw is only one of its members, so this is not sizeof().
constexpr size_t RenderItemStride = 8388608u / 65536u;

// Every submission carries exactly one draw with this declaration, overlay or not, so it cannot mark a scene.
constexpr uint32_t SharedVertexDeclIndex  = 4;
constexpr uint32_t InvalidVertexDeclIndex = 0xFFFFu;

// The 26.20 headers mis-align m_blitKeys/m_blitItem/m_frameCache, so walk from m_renderItem by declared sizes.
constexpr size_t FrameCounterOffsetFromRenderItem = 8388608u  // m_renderItem
                                                  + 88080384u // m_renderItemBind
                                                  + 524288u   // m_rangedRenderItemBind
                                                  + 4160u     // m_blitKeys
                                                  + 65600u    // m_blitItem
                                                  + 4718624u  // m_frameCache
                                                  + 8u;       // m_uniformBuffer

uint32_t readRenderItemCount(bgfx::Frame const* render) {
    auto const* base = reinterpret_cast<std::byte const*>(&render->m_renderItem[0].get());
    return *reinterpret_cast<uint32_t const*>(base + FrameCounterOffsetFromRenderItem);
}

// Only world geometry brings its own vertex formats; measured overlay submissions never do.
exporting::SceneSubmissionKind classifySubmission(bgfx::Frame const* render) {
    if (!render) return exporting::SceneSubmissionKind::OverlayOnly;
    auto const items = readRenderItemCount(render);
    if (items == 0 || items > 65536u) return exporting::SceneSubmissionKind::OverlayOnly;
    auto const* base = reinterpret_cast<std::byte const*>(&render->m_renderItem[0].get());
    for (uint32_t i = 0; i < items; ++i) {
        auto const& draw = *reinterpret_cast<bgfx::RenderDraw const*>(base + i * RenderItemStride);
        auto const  decl = static_cast<uint32_t>(draw.m_stream[0].get().m_decl.get().idx);
        if (decl != InvalidVertexDeclIndex && decl != SharedVertexDeclIndex) {
            return exporting::SceneSubmissionKind::Scene;
        }
    }
    return exporting::SceneSubmissionKind::OverlayOnly;
}

void recordNativeSwitchState(
    uint64_t                                 transition,
    std::string_view                         event,
    bgfx::d3d12::RendererContextD3D12 const* renderer,
    void const*                              frame   = nullptr,
    uint32_t                                 ordinal = 0
) noexcept {
    if (!transition) return;
    runDetourInstrumentation([&] {
        recordGraphicsSwitchTrace(
            transition,
            event,
            fmt::format(
                "renderer={} frame={} submit={} rtvHeap={} dsvHeap={} depth={} offline={}",
                static_cast<void const*>(renderer),
                frame,
                ordinal,
                static_cast<void*>(renderer->m_rtvDescriptorHeap),
                static_cast<void*>(renderer->m_dsvDescriptorHeap),
                static_cast<void*>(renderer->m_backBufferDepthStencil),
                exporting::isOfflineRenderActivityActive()
            )
        );
    });
}

LL_TYPE_INSTANCE_HOOK(
    GraphicsModeDiagnosticHook,
    ll::memory::HookPriority::Highest,
    MinecraftScreenModel,
    &MinecraftScreenModel::setGraphicsMode,
    void,
    int mode
) {
    ActiveRendererInitDetour activeDetour;
    uint64_t                 transition{};
    if (!gRendererInitHookStopping.load(std::memory_order_acquire) && exporting::renderDiagnosticsEnabled()) {
        runDetourInstrumentation([&] { transition = beginGraphicsSwitchTrace(getGraphicsMode(), mode); });
        gImGuiRenderer.recordGraphicsSwitchResources(transition, "Overlay.atModeSetterEnter");
    }
    origin(mode);
    if (transition) {
        runDetourInstrumentation([&] {
            recordGraphicsSwitchTrace(transition, "ModeSetter.return", fmt::format("selected={}", getGraphicsMode()));
        });
        gImGuiRenderer.recordGraphicsSwitchResources(transition, "Overlay.atModeSetterReturn");
    }
}

LL_TYPE_INSTANCE_HOOK(
    OfflineRenderSubmitHook,
    ll::memory::HookPriority::Highest,
    bgfx::d3d12::RendererContextD3D12,
    &bgfx::d3d12::RendererContextD3D12::$submit,
    void,
    bgfx::Frame*               render,
    bgfx::ClearQuad&           clearQuad,
    bgfx::TextVideoMemBlitter& textVideoMemBlitter
) {
    ActiveDetour activeDetour;
    auto const   entry = exporting::offlineRenderSceneSubmissionTicket();
    OfflineRenderTraceScope
        trace(OfflineRenderTraceEvent::SubmitD3D12Enter, OfflineRenderTraceEvent::SubmitD3D12Exit, this, render);
    // Only this hook sees the BGFX render thread submit world geometry; updateGraphics returning does not.
    bool const carriesScene = !gTimelineHooksStopping.load(std::memory_order_acquire)
                           && exporting::isOfflineRenderActivityActive()
                           && classifySubmission(render) == exporting::SceneSubmissionKind::Scene;
    auto const switchTicket = claimGraphicsSwitchSubmitTrace();
    recordNativeSwitchState(switchTicket.transition, "Submit.enter", this, render, switchTicket.ordinal);
    origin(render, clearQuad, textVideoMemBlitter);
    recordNativeSwitchState(switchTicket.transition, "Submit.return", this, render, switchTicket.ordinal);
    useSubmittedCameraProjection(render);
    bool const accepted = exporting::finishOfflineRenderSceneSubmission(entry, carriesScene);
    recordOfflineRenderTrace(
        OfflineRenderTraceEvent::SubmissionGate,
        render,
        this,
        entry.generation,
        exporting::offlineRenderSceneGeneration(),
        (carriesScene ? 1u : 0u) | (entry.cpuReady ? 2u : 0u),
        accepted ? 1 : 0
    );
    trace.result(carriesScene ? 1 : 0);
}

LL_TYPE_INSTANCE_HOOK(
    OfflineRenderSubmitD3D11Hook,
    ll::memory::HookPriority::Highest,
    bgfx::d3d11::RendererContextD3D11,
    &bgfx::d3d11::RendererContextD3D11::$submit,
    void,
    bgfx::Frame*               render,
    bgfx::ClearQuad&           clearQuad,
    bgfx::TextVideoMemBlitter& textVideoMemBlitter
) {
    ActiveDetour activeDetour;
    auto const   entry = exporting::offlineRenderSceneSubmissionTicket();
    OfflineRenderTraceScope
               trace(OfflineRenderTraceEvent::SubmitD3D11Enter, OfflineRenderTraceEvent::SubmitD3D11Exit, this, render);
    bool const carriesScene = !gTimelineHooksStopping.load(std::memory_order_acquire)
                           && exporting::isOfflineRenderActivityActive()
                           && classifySubmission(render) == exporting::SceneSubmissionKind::Scene;
    origin(render, clearQuad, textVideoMemBlitter);
    useSubmittedCameraProjection(render);
    bool const accepted = exporting::finishOfflineRenderSceneSubmission(entry, carriesScene);
    recordOfflineRenderTrace(
        OfflineRenderTraceEvent::SubmissionGate,
        render,
        this,
        entry.generation,
        exporting::offlineRenderSceneGeneration(),
        (carriesScene ? 1u : 0u) | (entry.cpuReady ? 2u : 0u),
        accepted ? 1 : 0
    );
    trace.result(carriesScene ? 1 : 0);
}

LL_TYPE_INSTANCE_HOOK(
    RendererPreResetHook,
    ll::memory::HookPriority::Highest,
    bgfx::d3d12::RendererContextD3D12,
    &bgfx::d3d12::RendererContextD3D12::preReset,
    void,
    bool swapChainReset
) {
    ActiveRendererInitDetour activeDetour;
    auto const               transition = currentGraphicsSwitchTrace();
    rearmGraphicsSwitchSubmitTrace(transition);
    recordNativeSwitchState(transition, "PreReset.enter", this);
    runDetourInstrumentation([&] {
        recordGraphicsSwitchTrace(transition, "PreReset.arguments", fmt::format("swapChainReset={}", swapChainReset));
    });
    if (!gRendererInitHookStopping.load(std::memory_order_acquire)) {
        if (exporting::renderDiagnosticsEnabled()) {
            getLogger().info("[RenderDiag] D3D12 preReset begin swapChainReset={}", swapChainReset);
        }
        bool const released = gImGuiRenderer.beforeRendererReset();
        if (!released) {
            getLogger().error("D3D12 reset: overlay GPU work did not drain; preview/capture remain suspended");
        } else if (exporting::renderDiagnosticsEnabled()) {
            getLogger().info("[RenderDiag] D3D12 preReset overlayReleased=true");
        }
    }
    origin(swapChainReset);
    recordNativeSwitchState(transition, "PreReset.return", this);
}

LL_TYPE_INSTANCE_HOOK(
    RendererPostResetHook,
    ll::memory::HookPriority::Highest,
    bgfx::d3d12::RendererContextD3D12,
    &bgfx::d3d12::RendererContextD3D12::postReset,
    void,
    bool swapChainReset
) {
    ActiveRendererInitDetour activeDetour;
    auto const               transition = currentGraphicsSwitchTrace();
    recordNativeSwitchState(transition, "PostReset.enter", this);
    origin(swapChainReset);
    recordNativeSwitchState(transition, "PostReset.return", this);
    rearmGraphicsSwitchSubmitTrace(transition);
    if (!gRendererInitHookStopping.load(std::memory_order_acquire)) {
        gImGuiRenderer.afterRendererReset();
        if (exporting::renderDiagnosticsEnabled()) {
            getLogger().info("[RenderDiag] D3D12 postReset complete swapChainReset={}", swapChainReset);
        }
    }
}

LL_TYPE_INSTANCE_HOOK(
    RendererShutdownHook,
    ll::memory::HookPriority::Highest,
    bgfx::d3d12::RendererContextD3D12,
    &bgfx::d3d12::RendererContextD3D12::$shutdown,
    void
) {
    ActiveRendererInitDetour activeDetour;
    auto const               transition = currentGraphicsSwitchTrace();
    recordNativeSwitchState(transition, "Shutdown.enter", this);
    gD3D12RendererActive.store(false, std::memory_order_release);
    if (!gRendererInitHookStopping.load(std::memory_order_acquire)) {
        bool const released = gImGuiRenderer.beforeRendererReset(true);
        if (!released) getLogger().error("D3D12 shutdown: overlay GPU work did not drain");
        if (exporting::renderDiagnosticsEnabled()) {
            getLogger().info("[RenderDiag] D3D12 shutdown overlayReleased={}", released);
        }
    }
    origin();
    recordGraphicsSwitchTrace(transition, "Shutdown.return");
}

LL_TYPE_INSTANCE_HOOK(
    RendererSuspendDiagnosticHook,
    ll::memory::HookPriority::Highest,
    bgfx::d3d12::RendererContextD3D12,
    &bgfx::d3d12::RendererContextD3D12::$suspend,
    void
) {
    ActiveRendererInitDetour activeDetour;
    auto const               transition = currentGraphicsSwitchTrace();
    rearmGraphicsSwitchSubmitTrace(transition);
    recordNativeSwitchState(transition, "Suspend.enter", this);
    origin();
    recordNativeSwitchState(transition, "Suspend.return", this);
}

LL_TYPE_INSTANCE_HOOK(
    RendererInitHook,
    ll::memory::HookPriority::Highest,
    bgfx::d3d12::RendererContextD3D12,
    &bgfx::d3d12::RendererContextD3D12::$init,
    bool,
    bgfx::Init const& init
) {
    ActiveRendererInitDetour activeDetour;
    if (!gRendererInitHookStopping.load(std::memory_order_acquire) && !hookD3D12(true)) {
        Playback::getInstance().getSelf().getLogger().error(
            "Unable to install replay ImGui timeline hooks before D3D12 renderer initialization"
        );
    }
    bool const initialized = origin(init);
    gD3D12RendererActive.store(initialized, std::memory_order_release);
    if (initialized && !gRendererInitHookStopping.load(std::memory_order_acquire)) {
        gImGuiRenderer.afterRendererReset();
    }
    return initialized;
}

} // namespace

bool isTimelineRenderingEnabled() { return !gTimelineHooksStopping.load(std::memory_order_acquire); }

bool isD3D12RendererActive() { return gD3D12RendererActive.load(std::memory_order_acquire); }

bool waitForActiveDetours() {
    std::unique_lock lock(gActiveDetoursMutex);
    return gActiveDetoursChanged.wait_for(lock, std::chrono::milliseconds(DetourWaitTimeoutMsLocal), [] {
        return gActiveDetours.load(std::memory_order_acquire) == 0;
    });
}

bool waitForActiveRendererInitDetours() {
    std::unique_lock lock(gActiveRendererInitDetoursMutex);
    return gActiveRendererInitDetoursChanged.wait_for(lock, std::chrono::milliseconds(DetourWaitTimeoutMsLocal), [] {
        return gActiveRendererInitDetours.load(std::memory_order_acquire) == 0;
    });
}

bool getDirectCommandQueue(IUnknown* object, ComPtr<ID3D12CommandQueue>& queue) {
    queue.Reset();
    if (!object || FAILED(object->QueryInterface(IID_PPV_ARGS(&queue)))) return false;
    if (queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) return true;
    queue.Reset();
    return false;
}

void bindSwapChainQueue(IDXGISwapChain* swapChain, IUnknown* queueObject) {
    if (!swapChain) return;
    ComPtr<ID3D12CommandQueue> queue;
    if (getDirectCommandQueue(queueObject, queue)) {
        swapChain->SetPrivateDataInterface(SwapChainQueueGuid, queue.Get());
    } else {
        swapChain->SetPrivateDataInterface(SwapChainQueueGuid, nullptr);
    }
}

ComPtr<ID3D12CommandQueue> getSwapChainQueue(IDXGISwapChain* swapChain) {
    if (!swapChain) return nullptr;
    ComPtr<IUnknown> queueObject;
    UINT             dataSize = sizeof(IUnknown*);
    if (SUCCEEDED(swapChain->GetPrivateData(SwapChainQueueGuid, &dataSize, queueObject.GetAddressOf()))
        && dataSize == sizeof(IUnknown*)) {
        ComPtr<ID3D12CommandQueue> queue;
        if (getDirectCommandQueue(queueObject.Get(), queue)) {
            return queue;
        }
    }
    return nullptr;
}

void unbindSwapChainQueue(IDXGISwapChain* swapChain) {
    if (swapChain) swapChain->SetPrivateDataInterface(SwapChainQueueGuid, nullptr);
}

ComPtr<ID3D12CommandQueue> getResizePresentQueue(UINT bufferCount, IUnknown* const* presentQueues) {
    if (!presentQueues || bufferCount == 0) return nullptr;
    ComPtr<ID3D12CommandQueue> selectedQueue;
    ComPtr<IUnknown>           selectedIdentity;
    for (UINT index = 0; index < bufferCount; ++index) {
        ComPtr<ID3D12CommandQueue> queue;
        if (!getDirectCommandQueue(presentQueues[index], queue)) return nullptr;
        ComPtr<IUnknown> identity;
        if (FAILED(queue.As(&identity))) return nullptr;
        if (!selectedQueue) {
            selectedQueue    = std::move(queue);
            selectedIdentity = std::move(identity);
        } else if (selectedIdentity.Get() != identity.Get()) {
            return nullptr;
        }
    }
    return selectedQueue;
}

bool resolveHookTargets(
    void*& outPresent,
    void*& outPresent1,
    void*& outResizeBuffers,
    void*& outResizeBuffers1,
    void*& outCreateSwapChain,
    void*& outCreateSwapChainForHwnd,
    void*& outCreateSwapChainForCoreWindow,
    void*& outCreateSwapChainForComposition
) {
    ComPtr<IDXGIFactory4> factory;
    HRESULT               result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        getLogger().error(
            "Unable to create the DXGI factory for replay timeline hooks (HRESULT=0x{:08X})",
            static_cast<uint32_t>(result)
        );
        return false;
    }

    ComPtr<ID3D12Device> device;
    result = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(result)) {
        getLogger().error(
            "Unable to create the D3D12 device for replay timeline hooks (HRESULT=0x{:08X})",
            static_cast<uint32_t>(result)
        );
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> commandQueue;
    result = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue));
    if (FAILED(result)) {
        getLogger().error(
            "Unable to create a D3D12 queue for replay timeline hooks (HRESULT=0x{:08X})",
            static_cast<uint32_t>(result)
        );
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.Width       = 2;
    swapChainDesc.Height      = 2;
    swapChainDesc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.SampleDesc  = {1, 0};
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = 2;
    swapChainDesc.Scaling     = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapChainDesc.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;

    ComPtr<IDXGISwapChain1> swapChain;
    result = factory->CreateSwapChainForComposition(commandQueue.Get(), &swapChainDesc, nullptr, &swapChain);
    if (FAILED(result)) {
        getLogger().error(
            "Unable to create the dummy DXGI swap chain for replay timeline hooks (HRESULT=0x{:08X})",
            static_cast<uint32_t>(result)
        );
        return false;
    }

    ComPtr<IDXGISwapChain3> swapChain3;
    result = swapChain.As(&swapChain3);
    if (FAILED(result)) {
        getLogger().error(
            "The dummy replay timeline swap chain does not expose IDXGISwapChain3 (HRESULT=0x{:08X})",
            static_cast<uint32_t>(result)
        );
        return false;
    }

    outPresent                       = getVtableEntry(swapChain3.Get(), SwapChainPresentIndex);
    outPresent1                      = getVtableEntry(swapChain3.Get(), SwapChainPresent1Index);
    outResizeBuffers                 = getVtableEntry(swapChain3.Get(), SwapChainResizeBuffersIndex);
    outResizeBuffers1                = getVtableEntry(swapChain3.Get(), SwapChainResizeBuffers1Index);
    outCreateSwapChain               = getVtableEntry(factory.Get(), FactoryCreateSwapChainIndex);
    outCreateSwapChainForHwnd        = getVtableEntry(factory.Get(), FactoryCreateSwapChainForHwndIndex);
    outCreateSwapChainForCoreWindow  = getVtableEntry(factory.Get(), FactoryCreateSwapChainForCoreWindowIndex);
    outCreateSwapChainForComposition = getVtableEntry(factory.Get(), FactoryCreateSwapChainForCompositionIndex);
    bool const resolved = outPresent && outPresent1 && outResizeBuffers && outResizeBuffers1 && outCreateSwapChain
                       && outCreateSwapChainForHwnd && outCreateSwapChainForCoreWindow
                       && outCreateSwapChainForComposition;
    if (!resolved) getLogger().error("One or more replay timeline DXGI vtable targets resolved to null");
    return resolved;
}

bool hookRendererInit(bool enable) {
    std::scoped_lock lock(gRendererInitHookMutex);
    static bool      initInstalled{};
    static bool      preResetInstalled{};
    static bool      postResetInstalled{};
    static bool      shutdownInstalled{};
    static bool      submitInstalled{};
    static bool      submitD3D11Installed{};
    static bool      modeDiagnosticInstalled{};
    static bool      suspendDiagnosticInstalled{};
    static bool      switchTraceOpened{};

    if (enable) {
        if (!initInstalled) {
            gRendererInitHookStopping.store(true, std::memory_order_release);
            if (RendererInitHook::hook() != 0) return false;
            initInstalled = true;
        }
        if (!preResetInstalled) {
            if (RendererPreResetHook::hook() != 0) return false;
            preResetInstalled = true;
        }
        if (!postResetInstalled) {
            if (RendererPostResetHook::hook() != 0) return false;
            postResetInstalled = true;
        }
        if (!shutdownInstalled) {
            if (RendererShutdownHook::hook() != 0) return false;
            shutdownInstalled = true;
        }
        // Both backend hooks are installed up front; only the backend BGFX actually selected will run.
        if (!submitInstalled) {
            if (OfflineRenderSubmitHook::hook() != 0) {
                getLogger().error("Unable to install the BGFX D3D12 scene submission capture hook");
                return false;
            }
            submitInstalled = true;
        }
        if (!submitD3D11Installed) {
            if (OfflineRenderSubmitD3D11Hook::hook() != 0) {
                getLogger().error("Unable to install the BGFX D3D11 scene submission capture hook");
                return false;
            }
            submitD3D11Installed = true;
        }

        if (exporting::renderDiagnosticsEnabled()) {
            if (!switchTraceOpened) {
                auto const path   = Playback::getInstance().getSelf().getModDir()
                                  / fmt::format("graphics-switch-{}-{}.log", GetCurrentProcessId(), GetTickCount64());
                switchTraceOpened = openGraphicsSwitchTrace(path);
                if (switchTraceOpened) getLogger().info("[RenderDiag] graphics switch journal={}", path);
                else getLogger().warn("Unable to open the graphics switch diagnostic journal");
            }
            if (switchTraceOpened && !modeDiagnosticInstalled) {
                modeDiagnosticInstalled = GraphicsModeDiagnosticHook::hook() == 0;
                if (!modeDiagnosticInstalled) getLogger().warn("Unable to install the graphics mode diagnostic hook");
            }
            if (switchTraceOpened && !suspendDiagnosticInstalled) {
                suspendDiagnosticInstalled = RendererSuspendDiagnosticHook::hook() == 0;
                if (!suspendDiagnosticInstalled)
                    getLogger().warn("Unable to install the renderer suspend diagnostic hook");
            }
            getLogger().info(
                "[RenderDiag] graphics switch hooks mode={} suspend={}",
                modeDiagnosticInstalled,
                suspendDiagnosticInstalled
            );
        }
        gRendererInitHookStopping.store(false, std::memory_order_release);
        return true;
    }

    gRendererInitHookStopping.store(true, std::memory_order_release);
    gD3D12RendererActive.store(false, std::memory_order_release);
    if (modeDiagnosticInstalled) {
        if (GraphicsModeDiagnosticHook::unhook()) modeDiagnosticInstalled = false;
        else return false;
    }
    if (suspendDiagnosticInstalled) {
        if (RendererSuspendDiagnosticHook::unhook()) suspendDiagnosticInstalled = false;
        else return false;
    }
    if (shutdownInstalled) {
        if (RendererShutdownHook::unhook()) shutdownInstalled = false;
        else return false;
    }
    if (postResetInstalled) {
        if (RendererPostResetHook::unhook()) postResetInstalled = false;
        else return false;
    }
    if (preResetInstalled) {
        if (RendererPreResetHook::unhook()) preResetInstalled = false;
        else return false;
    }
    if (submitD3D11Installed) {
        if (OfflineRenderSubmitD3D11Hook::unhook()) submitD3D11Installed = false;
        else return false;
    }
    if (submitInstalled) {
        if (OfflineRenderSubmitHook::unhook()) submitInstalled = false;
        else return false;
    }
    if (initInstalled) {
        if (RendererInitHook::unhook()) initInstalled = false;
        else return false;
    }
    if (!waitForActiveRendererInitDetours()) return false;
    if (!waitForActiveDetours()) return false;
    closeGraphicsSwitchTrace();
    switchTraceOpened = false;
    return true;
}

bool hookD3D12(bool enable) {
    std::scoped_lock lock(gTimelineHookMutex);
    auto&            state = hookState();

    if (enable) {
        if (coreInstalled(state)) {
            if (!captureInstalled(state) && !installCaptureHooks(state)) {
                bool const removed  = removeAll(state);
                bool const drained  = removed && waitForActiveDetours();
                bool const released = drained && gImGuiRenderer.shutdown();
                getLogger().error(
                    "Unable to restore the replay swap-chain queue hooks (removed={}, drained={}, cleanup={})",
                    removed,
                    drained,
                    released
                );
                return false;
            }
            gTimelineHooksStopping.store(false, std::memory_order_release);
            return true;
        }
        gTimelineHooksStopping.store(true, std::memory_order_release);
        if (!noneInstalled(state)) {
            if (!removeAll(state)) {
                getLogger().error("Unable to remove partially installed replay ImGui timeline hooks");
                return false;
            }
            if (!waitForActiveDetours()) {
                getLogger().error("Timed out while waiting for partial replay ImGui timeline detours to finish");
                return false;
            }
            if (!gImGuiRenderer.shutdown()) {
                getLogger().error("Unable to release replay ImGui timeline resources before reinstalling hooks");
                return false;
            }
        }
        if (!state.targets.present
            && !resolveHookTargets(
                state.targets.present,
                state.targets.present1,
                state.targets.resizeBuffers,
                state.targets.resizeBuffers1,
                state.targets.createSwapChain,
                state.targets.createSwapChainForHwnd,
                state.targets.createSwapChainForCoreWindow,
                state.targets.createSwapChainForComposition
            )) {
            getLogger().error("Unable to resolve D3D12 hook targets for the replay ImGui timeline");
            return false;
        }
        bool const captureOk = installCaptureHooks(state);
        if (!captureOk) {
            bool const removed  = removeAll(state);
            bool const drained  = removed && waitForActiveDetours();
            bool const released = drained && gImGuiRenderer.shutdown();
            getLogger().error(
                "Unable to install the replay swap-chain queue hooks (removed={}, drained={}, cleanup={})",
                removed,
                drained,
                released
            );
            return false;
        }
        if (installCoreHooks(state)) {
            gTimelineHooksStopping.store(false, std::memory_order_release);
            return true;
        }

        bool const removed  = removeAll(state);
        bool const drained  = removed && waitForActiveDetours();
        bool const released = drained && gImGuiRenderer.shutdown();
        getLogger().error(
            "Unable to install replay ImGui timeline hooks; rollback status "
            "(removed={}, drained={}, cleanup={}, present={}, present1={}, resize={}, resize1={}, "
            "create={}/{}/{}/{})",
            removed,
            drained,
            released,
            state.present,
            state.present1,
            state.resizeBuffers,
            state.resizeBuffers1,
            state.createSwapChain,
            state.createSwapChainForHwnd,
            state.createSwapChainForCoreWindow,
            state.createSwapChainForComposition
        );
        return false;
    }

    gTimelineHooksStopping.store(true, std::memory_order_release);
    if (!noneInstalled(state)) {
        if (!removeAll(state)) {
            getLogger().error("Unable to remove replay ImGui timeline hooks");
            return false;
        }
        if (!waitForActiveDetours()) {
            getLogger().error("Timed out while waiting for replay ImGui timeline detours to finish");
            return false;
        }
        if (!gImGuiRenderer.shutdown()) {
            getLogger().error("Unable to release replay ImGui timeline resources during shutdown");
            return false;
        }
    }
    return true;
}

} // namespace playback::editor::graphics
