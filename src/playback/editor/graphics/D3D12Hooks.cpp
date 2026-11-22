#include "D3D12Hooks.h"

#include "playback/Playback.h"
#include "playback/editor/graphics/CameraRenderHooks.h"
#include "playback/editor/graphics/ImGuiRenderer.h"
#include "playback/exporting/ExportActivity.h"
#include "playback/exporting/OfflineRenderClockHooks.h"

#include "ll/api/memory/Hook.h"

#include "mc/external/bgfx/Context.h"
#include "mc/external/bgfx/Frame.h"
#include "mc/external/bgfx/RenderDraw.h"
#include "mc/external/bgfx/RendererContextD3D11.h"
#include "mc/external/bgfx/RendererContextD3D12.h"


#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>

namespace playback::editor::graphics {

namespace {

constexpr DWORD DetourWaitTimeoutMsLocal = 2000;

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


constexpr uint32_t BgfxResetVsync         = 0x00000080u;
constexpr int32_t  BgfxFatalDeviceRemoved = 4;
constexpr int32_t  BgfxFatalCount         = 5;

struct BgfxResolutionLayout {
    uint32_t width{};
    uint32_t height{};
    uint32_t reset{};
};

struct BgfxFatalErrorLayout {
    int32_t type{};
    HRESULT hr{};
};

static_assert(sizeof(BgfxResolutionLayout) == sizeof(bgfx::d3d12::RendererContextD3D12::m_resolution));
static_assert(sizeof(BgfxResolutionLayout) == sizeof(bgfx::d3d11::RendererContextD3D11::m_resolution));
static_assert(sizeof(BgfxFatalErrorLayout) == sizeof(bgfx::d3d12::RendererContextD3D12::m_fatalError));
static_assert(sizeof(BgfxFatalErrorLayout) == sizeof(bgfx::d3d11::RendererContextD3D11::m_fatalError));

BgfxFatalErrorLayout readFatalError(bgfx::FatalError const& error) noexcept {
    BgfxFatalErrorLayout layout{};
    std::memcpy(&layout, &error, sizeof(layout));
    return layout;
}

class ScopedDisableVsync {
public:
    ScopedDisableVsync(bgfx::Resolution* resolution, bool disable) noexcept {
        if (!disable || !resolution || !exporting::isOfflineRenderActivityActive()) return;
        auto* const layout = reinterpret_cast<BgfxResolutionLayout*>(resolution);
        if ((layout->reset & BgfxResetVsync) == 0) return;
        mLayout         = layout;
        mPreviousReset  = layout->reset;
        layout->reset  &= ~BgfxResetVsync;
    }
    ~ScopedDisableVsync() {
        if (mLayout) mLayout->reset = mPreviousReset;
    }
    ScopedDisableVsync(ScopedDisableVsync const&)            = delete;
    ScopedDisableVsync& operator=(ScopedDisableVsync const&) = delete;

private:
    BgfxResolutionLayout* mLayout{};
    uint32_t              mPreviousReset{};
};

void ensureD3D12SwapChainQueue(bgfx::d3d12::RendererContextD3D12* ctx, IDXGISwapChain* swapChain) {
    if (!ctx || !swapChain || getSwapChainQueue(swapChain)) return;
    // m_cmd stores its active ID3D12CommandQueue* at offset 0.
    if (auto* const queuePtr = *reinterpret_cast<IUnknown* const*>(&ctx->m_cmd.get())) {
        ComPtr<ID3D12CommandQueue> queue;
        if (getDirectCommandQueue(queuePtr, queue)) {
            bindSwapChainQueue(swapChain, queue.Get());
        }
    }
}

bool renderPresentFrame(IDXGISwapChain* swapChain) {
    if (!swapChain) return false;
    // A native dimension wait pauses offline rendering mid-export; the export UI must stay on its window-sized layer.
    if (!exporting::isOfflineRenderActivityActive() && !exporting::isExportActivityActive()) {
        return gImGuiRenderer.render(swapChain);
    }
    // Captures before drawing the overlay, so the export frame holds the bare world.
    return gImGuiRenderer.renderExportOverlay(swapChain);
}

// m_renderItem is a 128-byte union slot and RenderDraw is only one of its members, so this is not sizeof().
constexpr size_t RenderItemStride = 8388608u / 65536u;

// Every submission carries exactly one draw with this declaration, overlay or not, so it cannot mark a scene.
constexpr uint32_t SharedVertexDeclIndex  = 4;
constexpr uint32_t InvalidVertexDeclIndex = 0xFFFFu;

// The SDK headers mis-align m_blitKeys/m_blitItem/m_frameCache, so walk from m_renderItem by declared sizes.
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
    // Only this hook sees the BGFX render thread submit world geometry; updateGraphics returning does not.
    bool const carriesScene = !gTimelineHooksStopping.load(std::memory_order_acquire)
                           && exporting::isOfflineRenderActivityActive()
                           && classifySubmission(render) == exporting::SceneSubmissionKind::Scene;
    origin(render, clearQuad, textVideoMemBlitter);
    useSubmittedCameraProjection(render);
    // The back buffer is complete here. Its Present only comes with the next frame the game sends, so capturing now
    // spares the export a second pass of the game loop for every frame.
    if (exporting::finishOfflineRenderSceneSubmission(entry, carriesScene)) {
        (void)gImGuiRenderer.captureSubmittedScene();
    }
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
    auto const   entry        = exporting::offlineRenderSceneSubmissionTicket();
    bool const   carriesScene = !gTimelineHooksStopping.load(std::memory_order_acquire)
                           && exporting::isOfflineRenderActivityActive()
                           && classifySubmission(render) == exporting::SceneSubmissionKind::Scene;
    origin(render, clearQuad, textVideoMemBlitter);
    useSubmittedCameraProjection(render);
    (void)exporting::finishOfflineRenderSceneSubmission(entry, carriesScene);
}

LL_TYPE_INSTANCE_HOOK(
    RendererFlipHook,
    ll::memory::HookPriority::Highest,
    bgfx::d3d12::RendererContextD3D12,
    &bgfx::d3d12::RendererContextD3D12::$flip,
    void
) {
    ActiveDetour    activeDetour;
    IDXGISwapChain* swapChain{};
    bool            hadDeviceLost{};
    bool            disableVsync{};
    runDetourInstrumentation([&] {
        auto const fatalError = readFatalError(this->m_fatalError.get());
        hadDeviceLost         = fatalError.type == BgfxFatalDeviceRemoved;
        if (gTimelineHooksStopping.load(std::memory_order_acquire)
            || gRendererInitHookStopping.load(std::memory_order_acquire)) {
            return;
        }
        swapChain = static_cast<IDXGISwapChain*>(this->m_swapChain);
        if (!swapChain || this->m_fatal || fatalError.type < BgfxFatalCount) return;
        gD3D12RendererActive.store(true, std::memory_order_release);
        ensureD3D12SwapChainQueue(this, swapChain);
        (void)renderPresentFrame(swapChain);
        disableVsync = exporting::isOfflineRenderActivityActive();
    });
    {
        ScopedDisableVsync vsyncGuard(&this->m_resolution.get(), disableVsync);
        origin();
    }
    runDetourInstrumentation([&] {
        if (!swapChain || hadDeviceLost) return;
        auto const fatalError = readFatalError(this->m_fatalError.get());
        if (fatalError.type == BgfxFatalDeviceRemoved) {
            gImGuiRenderer.onDeviceLost(swapChain, fatalError.hr);
        }
    });
}

LL_TYPE_INSTANCE_HOOK(
    RendererFlipD3D11Hook,
    ll::memory::HookPriority::Highest,
    bgfx::d3d11::RendererContextD3D11,
    &bgfx::d3d11::RendererContextD3D11::$flip,
    void
) {
    ActiveDetour    activeDetour;
    IDXGISwapChain* swapChain{};
    bool            hadDeviceLost{};
    bool            disableVsync{};
    runDetourInstrumentation([&] {
        auto const fatalError = readFatalError(this->m_fatalError.get());
        hadDeviceLost         = fatalError.type == BgfxFatalDeviceRemoved;
        if (gTimelineHooksStopping.load(std::memory_order_acquire)
            || gRendererInitHookStopping.load(std::memory_order_acquire)) {
            return;
        }
        swapChain = static_cast<IDXGISwapChain*>(this->m_swapChain);
        if (!swapChain || this->m_fatal || !this->m_needPresent || fatalError.type < BgfxFatalCount) return;
        gD3D12RendererActive.store(false, std::memory_order_release);
        (void)renderPresentFrame(swapChain);
        disableVsync = exporting::isOfflineRenderActivityActive();
    });
    {
        ScopedDisableVsync vsyncGuard(&this->m_resolution.get(), disableVsync);
        origin();
    }
    runDetourInstrumentation([&] {
        if (!swapChain || hadDeviceLost) return;
        auto const fatalError = readFatalError(this->m_fatalError.get());
        if (fatalError.type == BgfxFatalDeviceRemoved) {
            gImGuiRenderer.onDeviceLost(swapChain, fatalError.hr);
        }
    });
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
    if (!gRendererInitHookStopping.load(std::memory_order_acquire) && !gImGuiRenderer.beforeRendererReset()) {
        getLogger().error("D3D12 reset: overlay GPU work did not drain; preview/capture remain suspended");
    }
    origin(swapChainReset);
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
    origin(swapChainReset);
    if (!gRendererInitHookStopping.load(std::memory_order_acquire)) {
        if (auto* const swapChain = static_cast<IDXGISwapChain*>(this->m_swapChain)) {
            ensureD3D12SwapChainQueue(this, swapChain);
        }
        gImGuiRenderer.afterRendererReset();
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
    gD3D12RendererActive.store(false, std::memory_order_release);
    if (!gRendererInitHookStopping.load(std::memory_order_acquire) && !gImGuiRenderer.beforeRendererReset(true)) {
        getLogger().error("D3D12 shutdown: overlay GPU work did not drain");
    }
    if (auto* const swapChain = static_cast<IDXGISwapChain*>(this->m_swapChain)) {
        unbindSwapChainQueue(swapChain);
    }
    origin();
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
    if (initialized) {
        if (auto* const swapChain = static_cast<IDXGISwapChain*>(this->m_swapChain)) {
            ensureD3D12SwapChainQueue(this, swapChain);
        }
        if (!gRendererInitHookStopping.load(std::memory_order_acquire)) {
            gImGuiRenderer.afterRendererReset();
        }
    }
    return initialized;
}

LL_TYPE_INSTANCE_HOOK(
    RendererD3D11PreResetHook,
    ll::memory::HookPriority::Highest,
    bgfx::d3d11::RendererContextD3D11,
    &bgfx::d3d11::RendererContextD3D11::preReset,
    void
) {
    ActiveRendererInitDetour activeDetour;
    if (!gRendererInitHookStopping.load(std::memory_order_acquire) && !gImGuiRenderer.beforeRendererReset()) {
        getLogger().error("D3D11 reset: overlay GPU work did not drain; preview/capture remain suspended");
    }
    origin();
}

LL_TYPE_INSTANCE_HOOK(
    RendererD3D11PostResetHook,
    ll::memory::HookPriority::Highest,
    bgfx::d3d11::RendererContextD3D11,
    &bgfx::d3d11::RendererContextD3D11::postReset,
    void
) {
    ActiveRendererInitDetour activeDetour;
    origin();
    if (!gRendererInitHookStopping.load(std::memory_order_acquire)) gImGuiRenderer.afterRendererReset();
}

LL_TYPE_INSTANCE_HOOK(
    BgfxShutdownBeginHook,
    ll::memory::HookPriority::Highest,
    bgfx::Context,
    &bgfx::Context::_shutdownBegin,
    void
) {
    ActiveRendererInitDetour activeDetour;
    if (!gRendererInitHookStopping.load(std::memory_order_acquire) && !gImGuiRenderer.beforeRendererReset(true)) {
        getLogger().error("BGFX shutdown: overlay GPU work did not drain");
    }
    origin();
}

LL_TYPE_INSTANCE_HOOK(
    BgfxInitFinalizeHook,
    ll::memory::HookPriority::Highest,
    bgfx::Context,
    &bgfx::Context::_initFinalize,
    bool
) {
    ActiveRendererInitDetour activeDetour;
    if (!gRendererInitHookStopping.load(std::memory_order_acquire) && !hookD3D12(true)) {
        Playback::getInstance().getSelf().getLogger().error(
            "Unable to install replay ImGui timeline hooks before BGFX renderer initialization"
        );
    }
    bool const initialized = origin();
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

bool hookRendererInit(bool enable) {
    std::scoped_lock lock(gRendererInitHookMutex);
    static bool      initInstalled{};
    static bool      bgfxInitFinalizeInstalled{};
    static bool      preResetInstalled{};
    static bool      preResetD3D11Installed{};
    static bool      postResetInstalled{};
    static bool      postResetD3D11Installed{};
    static bool      shutdownInstalled{};
    static bool      bgfxShutdownBeginInstalled{};
    static bool      submitInstalled{};
    static bool      submitD3D11Installed{};
    static bool      flipInstalled{};
    static bool      flipD3D11Installed{};

    if (enable) {
        if (!initInstalled) {
            gRendererInitHookStopping.store(true, std::memory_order_release);
            if (RendererInitHook::hook() != 0) return false;
            initInstalled = true;
        }
        if (!bgfxInitFinalizeInstalled) {
            if (BgfxInitFinalizeHook::hook() != 0) return false;
            bgfxInitFinalizeInstalled = true;
        }
        if (!preResetInstalled) {
            if (RendererPreResetHook::hook() != 0) return false;
            preResetInstalled = true;
        }
        if (!preResetD3D11Installed) {
            if (RendererD3D11PreResetHook::hook() != 0) return false;
            preResetD3D11Installed = true;
        }
        if (!postResetInstalled) {
            if (RendererPostResetHook::hook() != 0) return false;
            postResetInstalled = true;
        }
        if (!postResetD3D11Installed) {
            if (RendererD3D11PostResetHook::hook() != 0) return false;
            postResetD3D11Installed = true;
        }
        if (!shutdownInstalled) {
            if (RendererShutdownHook::hook() != 0) return false;
            shutdownInstalled = true;
        }
        if (!bgfxShutdownBeginInstalled) {
            if (BgfxShutdownBeginHook::hook() != 0) return false;
            bgfxShutdownBeginInstalled = true;
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
        if (!flipInstalled) {
            if (RendererFlipHook::hook() != 0) {
                getLogger().error("Unable to install the BGFX D3D12 flip overlay hook");
                return false;
            }
            flipInstalled = true;
        }
        if (!flipD3D11Installed) {
            if (RendererFlipD3D11Hook::hook() != 0) {
                getLogger().error("Unable to install the BGFX D3D11 flip overlay hook");
                return false;
            }
            flipD3D11Installed = true;
        }

        gRendererInitHookStopping.store(false, std::memory_order_release);
        return true;
    }

    gRendererInitHookStopping.store(true, std::memory_order_release);
    gD3D12RendererActive.store(false, std::memory_order_release);
    if (flipD3D11Installed) {
        if (RendererFlipD3D11Hook::unhook()) flipD3D11Installed = false;
        else return false;
    }
    if (flipInstalled) {
        if (RendererFlipHook::unhook()) flipInstalled = false;
        else return false;
    }
    if (bgfxShutdownBeginInstalled) {
        if (BgfxShutdownBeginHook::unhook()) bgfxShutdownBeginInstalled = false;
        else return false;
    }
    if (shutdownInstalled) {
        if (RendererShutdownHook::unhook()) shutdownInstalled = false;
        else return false;
    }
    if (postResetD3D11Installed) {
        if (RendererD3D11PostResetHook::unhook()) postResetD3D11Installed = false;
        else return false;
    }
    if (postResetInstalled) {
        if (RendererPostResetHook::unhook()) postResetInstalled = false;
        else return false;
    }
    if (preResetD3D11Installed) {
        if (RendererD3D11PreResetHook::unhook()) preResetD3D11Installed = false;
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
    if (bgfxInitFinalizeInstalled) {
        if (BgfxInitFinalizeHook::unhook()) bgfxInitFinalizeInstalled = false;
        else return false;
    }
    if (initInstalled) {
        if (RendererInitHook::unhook()) initInstalled = false;
        else return false;
    }
    if (!waitForActiveRendererInitDetours()) return false;
    return waitForActiveDetours();
}

bool hookD3D12(bool enable) {
    std::scoped_lock lock(gTimelineHookMutex);
    if (enable) {
        gTimelineHooksStopping.store(false, std::memory_order_release);
        return true;
    }

    gTimelineHooksStopping.store(true, std::memory_order_release);
    if (!waitForActiveDetours()) {
        getLogger().error("Timed out while waiting for replay ImGui timeline detours to finish");
        return false;
    }
    if (!gImGuiRenderer.shutdown()) {
        getLogger().error("Unable to release replay ImGui timeline resources during shutdown");
        return false;
    }
    return true;
}

} // namespace playback::editor::graphics
