#include "D3D12Hooks.h"

#include "playback/Playback.h"
#include "playback/editor/renderer/D3D12Compat.h"
#include "playback/editor/renderer/ImGuiRenderer.h"


#include "ll/api/memory/Hook.h"

#include "mc/external/bgfx/RendererContextD3D12.h"

#include <d3d11.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace playback::editor::renderer {

namespace {

using PresentFn       = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn      = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, DXGI_PRESENT_PARAMETERS const*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Fn =
    HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, UINT const*, IUnknown* const*);
using CreateSwapChainFn =
    HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn =
    HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, DXGI_SWAP_CHAIN_DESC1 const*, DXGI_SWAP_CHAIN_FULLSCREEN_DESC const*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainForCoreWindowFn =
    HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, DXGI_SWAP_CHAIN_DESC1 const*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainForCompositionFn =
    HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, DXGI_SWAP_CHAIN_DESC1 const*, IDXGIOutput*, IDXGISwapChain1**);
using CreateCommandQueueFn =
    HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_COMMAND_QUEUE_DESC const*, REFIID, void**);

constexpr size_t SwapChainPresentIndex                     = 8;
constexpr size_t SwapChainPresent1Index                    = 22;
constexpr size_t SwapChainResizeBuffersIndex               = 13;
constexpr size_t SwapChainResizeBuffers1Index              = 39;
constexpr size_t FactoryCreateSwapChainIndex               = 10;
constexpr size_t FactoryCreateSwapChainForHwndIndex        = 15;
constexpr size_t FactoryCreateSwapChainForCoreWindowIndex  = 16;
constexpr size_t FactoryCreateSwapChainForCompositionIndex = 24;
constexpr size_t DeviceCreateCommandQueueIndex             = 8;
constexpr DWORD  DetourWaitTimeoutMsLocal                  = 2000;

ll::memory::FuncPtr gOriginalPresent{};
ll::memory::FuncPtr gOriginalPresent1{};
ll::memory::FuncPtr gOriginalResizeBuffers{};
ll::memory::FuncPtr gOriginalResizeBuffers1{};
ll::memory::FuncPtr gOriginalCreateSwapChain{};
ll::memory::FuncPtr gOriginalCreateSwapChainForHwnd{};
ll::memory::FuncPtr gOriginalCreateSwapChainForCoreWindow{};
ll::memory::FuncPtr gOriginalCreateSwapChainForComposition{};
ll::memory::FuncPtr gOriginalCreateCommandQueue{};

// A device fallback is safe only while exactly one Direct queue has been observed.
struct DeviceQueueCandidate {
    ComPtr<ID3D12CommandQueue> queue;
    bool                       ambiguous{};
};

std::mutex                                                    gDeviceQueueMapMutex;
std::unordered_map<ID3D12Device*, DeviceQueueCandidate>       gDeviceQueueMap;

std::atomic<bool>   gTimelineHooksStopping{true};
std::atomic<bool>   gRendererInitHookStopping{true};

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
    void* createCommandQueue{};
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
    bool        createCommandQueue{};
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
        && state.createSwapChainForComposition && state.createCommandQueue;
}

bool noneInstalled(HookState const& state) {
    return !state.present && !state.present1 && !state.resizeBuffers && !state.resizeBuffers1 && !state.createSwapChain
        && !state.createSwapChainForHwnd && !state.createSwapChainForCoreWindow && !state.createSwapChainForComposition
        && !state.createCommandQueue;
}

#define DECLARE_DETOUR_FN(NAME, RET, ...)                                                                              \
    RET WINAPI NAME##Detour(__VA_ARGS__);                                                                              \
    RET WINAPI NAME##Detour(__VA_ARGS__)

DECLARE_DETOUR_FN(present, HRESULT, IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    // Log every unique swap chain we see (throttled)
    {
        static std::unordered_map<IDXGISwapChain*, bool> sSeen;
        static std::once_flag sPresentLog;
        bool first = false;
        {
            static std::mutex sMtx;
            std::scoped_lock lk(sMtx);
            auto it = sSeen.find(swapChain);
            if (it == sSeen.end()) {
                sSeen[swapChain] = true;
                first = true;
            }
        }
        if (first) {
            // #region debug-point imgui-swapchain-present-type
            ComPtr<ID3D12Device> d3d12Device;
            ComPtr<ID3D11Device> d3d11Device;
            ComPtr<IDXGIDevice>  dxgiDevice;
            HRESULT const        d3d12Result = swapChain->GetDevice(IID_PPV_ARGS(&d3d12Device));
            HRESULT const        d3d11Result = swapChain->GetDevice(IID_PPV_ARGS(&d3d11Device));
            HRESULT const        dxgiResult  = swapChain->GetDevice(IID_PPV_ARGS(&dxgiDevice));
            DXGI_SWAP_CHAIN_DESC swapChainDesc{};
            HRESULT const        descResult = swapChain->GetDesc(&swapChainDesc);
            FILE*                file       = nullptr;
            fopen_s(&file, "mods/playback/debug_log.txt", "a");
            if (file) {
                fprintf(
                    file,
                    "[DebugApi] present swapChain=%p d3d12=0x%08X/%p d3d11=0x%08X/%p dxgi=0x%08X/%p "
                    "desc=0x%08X size=%ux%u format=%u hwnd=%p\n",
                    static_cast<void*>(swapChain),
                    static_cast<unsigned int>(d3d12Result),
                    static_cast<void*>(d3d12Device.Get()),
                    static_cast<unsigned int>(d3d11Result),
                    static_cast<void*>(d3d11Device.Get()),
                    static_cast<unsigned int>(dxgiResult),
                    static_cast<void*>(dxgiDevice.Get()),
                    static_cast<unsigned int>(descResult),
                    swapChainDesc.BufferDesc.Width,
                    swapChainDesc.BufferDesc.Height,
                    static_cast<unsigned int>(swapChainDesc.BufferDesc.Format),
                    static_cast<void*>(swapChainDesc.OutputWindow)
                );
                fclose(file);
            }
            // #endregion debug-point imgui-swapchain-present-type
        }
    }
    ActiveDetour activeDetour;
    if ((flags & DXGI_PRESENT_TEST) == 0 && !gTimelineHooksStopping.load(std::memory_order_acquire)) {
        (void)gImGuiRenderer.render(swapChain);
    }
    HRESULT const result = reinterpret_cast<PresentFn>(gOriginalPresent)(swapChain, syncInterval, flags);
    gImGuiRenderer.afterPresent(swapChain, result);
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
    ActiveDetour activeDetour;
    bool         timelineRendered = false;
    if ((flags & DXGI_PRESENT_TEST) == 0 && !gTimelineHooksStopping.load(std::memory_order_acquire)) {
        timelineRendered = gImGuiRenderer.render(swapChain);
    }

    DXGI_PRESENT_PARAMETERS fullSurfacePresent{};
    auto const*             effectiveParameters = parameters;
    if (timelineRendered && parameters) {
        fullSurfacePresent                 = *parameters;
        fullSurfacePresent.DirtyRectsCount = 0;
        fullSurfacePresent.pDirtyRects     = nullptr;
        fullSurfacePresent.pScrollRect     = nullptr;
        fullSurfacePresent.pScrollOffset   = nullptr;
        effectiveParameters                = &fullSurfacePresent;
    }

    HRESULT const result =
        reinterpret_cast<Present1Fn>(gOriginalPresent1)(swapChain, syncInterval, flags, effectiveParameters);
    gImGuiRenderer.afterPresent(swapChain, result);
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
    if (!gImGuiRenderer.beforeResize(swapChain)) return DXGI_ERROR_INVALID_CALL;
    return reinterpret_cast<ResizeBuffersFn>(gOriginalResizeBuffers)(
        swapChain,
        bufferCount,
        width,
        height,
        format,
        flags
    );
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
    ActiveDetour                     activeDetour;
    ComPtr<ID3D12CommandQueue> const queue               = getResizePresentQueue(bufferCount, presentQueue);
    bool const                       updatesQueueBinding = bufferCount > 0 && presentQueue;
    if (!gImGuiRenderer.beforeResize(swapChain)) return DXGI_ERROR_INVALID_CALL;
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
        if (queue) bindSwapChainQueue(swapChain, queue.Get());
        else unbindSwapChainQueue(swapChain);
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
    static std::once_flag sLog;
    std::call_once(sLog, [] { FILE* f = nullptr; fopen_s(&f, "mods/playback/debug_log.txt", "a"); if(f){fprintf(f,"[Hook] createSwapChain detour HIT\n");fclose(f);} });
    ActiveDetour activeDetour;
    HRESULT const result =
        reinterpret_cast<CreateSwapChainFn>(gOriginalCreateSwapChain)(factory, device, description, swapChain);
    if (SUCCEEDED(result) && swapChain && *swapChain) {
        static std::once_flag sCsOk;
        std::call_once(sCsOk, [=] { FILE* f = nullptr; fopen_s(&f, "mods/playback/debug_log.txt", "a"); if(f){fprintf(f,"[Hook] CreateSwapChain SUCCEEDED, calling bindSwapChainQueue\n");fclose(f);} });
        bindSwapChainQueue(*swapChain, device);
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
    static std::once_flag sLog;
    std::call_once(sLog, [] { FILE* f = nullptr; fopen_s(&f, "mods/playback/debug_log.txt", "a"); if(f){fprintf(f,"[Hook] createSwapChainForHwnd detour HIT\n");fclose(f);} });
    // #region debug-point imgui-swapchain-create-type
    {
        ComPtr<ID3D12CommandQueue> d3d12Queue;
        ComPtr<ID3D12Device>       d3d12Device;
        ComPtr<ID3D11Device>       d3d11Device;
        ComPtr<IDXGIDevice>        dxgiDevice;
        HRESULT const queueResult = device ? device->QueryInterface(IID_PPV_ARGS(&d3d12Queue)) : E_POINTER;
        HRESULT const d3d12Result = device ? device->QueryInterface(IID_PPV_ARGS(&d3d12Device)) : E_POINTER;
        HRESULT const d3d11Result = device ? device->QueryInterface(IID_PPV_ARGS(&d3d11Device)) : E_POINTER;
        HRESULT const dxgiResult  = device ? device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)) : E_POINTER;
        FILE*         file        = nullptr;
        fopen_s(&file, "mods/playback/debug_log.txt", "a");
        if (file) {
            fprintf(
                file,
                "[DebugApi] create hwnd device=%p queue=0x%08X/%p d3d12=0x%08X/%p d3d11=0x%08X/%p "
                "dxgi=0x%08X/%p size=%ux%u format=%u hwnd=%p\n",
                static_cast<void*>(device),
                static_cast<unsigned int>(queueResult),
                static_cast<void*>(d3d12Queue.Get()),
                static_cast<unsigned int>(d3d12Result),
                static_cast<void*>(d3d12Device.Get()),
                static_cast<unsigned int>(d3d11Result),
                static_cast<void*>(d3d11Device.Get()),
                static_cast<unsigned int>(dxgiResult),
                static_cast<void*>(dxgiDevice.Get()),
                description ? description->Width : 0,
                description ? description->Height : 0,
                description ? static_cast<unsigned int>(description->Format) : 0,
                static_cast<void*>(window)
            );
            fclose(file);
        }
    }
    // #endregion debug-point imgui-swapchain-create-type
    ActiveDetour activeDetour;
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
        static std::once_flag sOk;
        std::call_once(sOk, [=] { FILE* f = nullptr; fopen_s(&f, "mods/playback/debug_log.txt", "a"); if(f){fprintf(f,"[Hook] CreateSwapChainForHwnd SUCCEEDED, binding queue\n");fclose(f);} });
        bindSwapChainQueue(*swapChain, device);
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
    static std::once_flag sLog;
    std::call_once(sLog, [] { FILE* f = nullptr; fopen_s(&f, "mods/playback/debug_log.txt", "a"); if(f){fprintf(f,"[Hook] createSwapChainForCoreWindow detour HIT\n");fclose(f);} });
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
        static std::once_flag sOk;
        std::call_once(sOk, [=] { FILE* f = nullptr; fopen_s(&f, "mods/playback/debug_log.txt", "a"); if(f){fprintf(f,"[Hook] CreateSwapChainForCoreWindow SUCCEEDED\n");fclose(f);} });
        bindSwapChainQueue(*swapChain, device);
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
    static std::once_flag sLog;
    std::call_once(sLog, [] { FILE* f = nullptr; fopen_s(&f, "mods/playback/debug_log.txt", "a"); if(f){fprintf(f,"[Hook] createSwapChainForComposition detour HIT\n");fclose(f);} });
    ActiveDetour  activeDetour;
    HRESULT const result = reinterpret_cast<CreateSwapChainForCompositionFn>(gOriginalCreateSwapChainForComposition)(
        factory,
        device,
        description,
        restrictToOutput,
        swapChain
    );
    if (SUCCEEDED(result) && swapChain && *swapChain) {
        static std::once_flag sOk;
        std::call_once(sOk, [=] { FILE* f = nullptr; fopen_s(&f, "mods/playback/debug_log.txt", "a"); if(f){fprintf(f,"[Hook] CreateSwapChainForComposition SUCCEEDED\n");fclose(f);} });
        bindSwapChainQueue(*swapChain, device);
    }
    return result;
}

DECLARE_DETOUR_FN(
    createCommandQueue,
    HRESULT,
    ID3D12Device*                   device,
    D3D12_COMMAND_QUEUE_DESC const* desc,
    REFIID                          riid,
    void**                          ppCommandQueue
) {
    static std::once_flag sLog;
    std::call_once(sLog, [] { FILE* f = nullptr; fopen_s(&f, "mods/playback/debug_log.txt", "a"); if(f){fprintf(f,"[Hook] createCommandQueue detour HIT\n");fclose(f);} });
    HRESULT const result = reinterpret_cast<CreateCommandQueueFn>(gOriginalCreateCommandQueue)(device, desc, riid, ppCommandQueue);
    if (FAILED(result) || !desc || desc->Type != D3D12_COMMAND_LIST_TYPE_DIRECT || !ppCommandQueue || !*ppCommandQueue) {
        return result;
    }

    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(reinterpret_cast<IUnknown*>(*ppCommandQueue)->QueryInterface(IID_PPV_ARGS(&queue)))) return result;

    ComPtr<IUnknown> newIdentity;
    if (FAILED(queue.As(&newIdentity))) return result;

    bool ambiguous = false;
    {
        std::scoped_lock lock(gDeviceQueueMapMutex);
        auto&            candidate = gDeviceQueueMap[device];
        if (!candidate.queue) {
            candidate.queue = queue;
        } else {
            ComPtr<IUnknown> currentIdentity;
            if (FAILED(candidate.queue.As(&currentIdentity)) || currentIdentity.Get() != newIdentity.Get()) {
                candidate.ambiguous = true;
            }
        }
        ambiguous = candidate.ambiguous;
    }

    FILE* file = nullptr;
    fopen_s(&file, "mods/playback/debug_log.txt", "a");
    if (file) {
        fprintf(
            file,
            "[Queue] captured Direct queue device=%p queue=%p ambiguous=%d\n",
            static_cast<void*>(device),
            static_cast<void*>(queue.Get()),
            ambiguous
        );
        fclose(file);
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

bool installCreateCommandQueueHook(HookState& state) {
    if (state.createCommandQueue) return true;
    if (ll::memory::hook(
            state.targets.createCommandQueue,
            ll::memory::toFuncPtr(&createCommandQueueDetour),
            &gOriginalCreateCommandQueue,
            ll::memory::HookPriority::Normal
        )
        != 0)
        return false;
    state.createCommandQueue = true;
    return true;
}

bool installCoreHooks(HookState& state) {
    return installResizeBuffersHook(state) && installResizeBuffers1Hook(state) && installPresentHook(state)
        && installPresent1Hook(state);
}

bool installCaptureHooks(HookState& state) {
    bool ok = true;
    ok      = installCreateCommandQueueHook(state) && ok;
    ok      = installCreateSwapChainHook(state) && ok;
    ok      = installCreateSwapChainForHwndHook(state) && ok;
    ok      = installCreateSwapChainForCoreWindowHook(state) && ok;
    ok      = installCreateSwapChainForCompositionHook(state) && ok;
    return ok;
}

bool removeAll(HookState& state) {
    if (state.createCommandQueue
        && ll::memory::unhook(state.targets.createCommandQueue, ll::memory::toFuncPtr(&createCommandQueueDetour)))
        state.createCommandQueue = false;
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

LL_TYPE_INSTANCE_HOOK(
    RendererInitHook,
    ll::memory::HookPriority::Highest,
    bgfx::d3d12::RendererContextD3D12,
    &bgfx::d3d12::RendererContextD3D12::$init,
    bool,
    bgfx::Init const& init
) {
    ActiveRendererInitDetour activeDetour;
    {
        FILE* f = nullptr; fopen_s(&f, "mods/playback/debug_log.txt", "a"); if(f){fprintf(f,"[Hook] RendererInitHook FIRED\n");fclose(f);}
    }
    if (!gRendererInitHookStopping.load(std::memory_order_acquire) && !hookD3D12(true)) {
        Playback::getInstance().getSelf().getLogger().error(
            "Unable to install replay ImGui timeline hooks before D3D12 renderer initialization"
        );
    }
    return origin(init);
}

} // namespace

bool isTimelineRenderingEnabled() { return !gTimelineHooksStopping.load(std::memory_order_acquire); }

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

// ── Swap chain queue fallback map (used when private data is lost) ──
std::mutex                         gSwapChainQueueFallbackMutex;
std::unordered_map<void*, IUnknown*> gSwapChainQueueFallback;

void bindSwapChainQueue(IDXGISwapChain* swapChain, IUnknown* queueObject) {
    if (!swapChain) return;
    ComPtr<ID3D12CommandQueue> queue;
    if (getDirectCommandQueue(queueObject, queue)) {
        swapChain->SetPrivateDataInterface(SwapChainQueueGuid, queue.Get());
        // Also retain the exact queue for this swap chain if DXGI private data is lost.
        std::scoped_lock lock(gSwapChainQueueFallbackMutex);
        auto&            retained = gSwapChainQueueFallback[swapChain];
        if (retained) retained->Release();
        retained = queue.Detach();
        {
            FILE* f = nullptr; fopen_s(&f, "mods/playback/debug_log.txt", "a"); if(f){fprintf(f,"[Hook] bindSwapChainQueue SUCCESS for swapChain=%p\n", (void*)swapChain);fclose(f);}
        }
    } else {
        swapChain->SetPrivateDataInterface(SwapChainQueueGuid, nullptr);
        {
            FILE* f = nullptr; fopen_s(&f, "mods/playback/debug_log.txt", "a"); if(f){fprintf(f,"[Hook] bindSwapChainQueue FAILED: getDirectCommandQueue returned false\n");fclose(f);}
        }
    }
}

ComPtr<ID3D12CommandQueue> getSwapChainQueue(IDXGISwapChain* swapChain) {
    if (!swapChain) return nullptr;
    // Primary path: private data set by the detour
    ComPtr<IUnknown> queueObject;
    UINT             dataSize = sizeof(IUnknown*);
    if (SUCCEEDED(swapChain->GetPrivateData(SwapChainQueueGuid, &dataSize, queueObject.GetAddressOf()))
        && dataSize == sizeof(IUnknown*))
    {
        ComPtr<ID3D12CommandQueue> queue;
        if (getDirectCommandQueue(queueObject.Get(), queue)) {
            static std::once_flag sOk;
            std::call_once(sOk, [] { FILE* f = nullptr; fopen_s(&f, "mods/playback/debug_log.txt", "a"); if(f){fprintf(f,"[Hook] getSwapChainQueue: found via private data\n");fclose(f);} });
            return queue;
        }
    }
    // Fallback: global map (survives IFramebuffer recreation etc.)
    {
        std::scoped_lock lk(gSwapChainQueueFallbackMutex);
        auto it = gSwapChainQueueFallback.find(swapChain);
        if (it != gSwapChainQueueFallback.end() && it->second) {
            ComPtr<ID3D12CommandQueue> queue;
            if (SUCCEEDED(it->second->QueryInterface(IID_PPV_ARGS(&queue))) && queue) {
                static std::once_flag sFallback;
                std::call_once(sFallback, [] { FILE* f = nullptr; fopen_s(&f, "mods/playback/debug_log.txt", "a"); if(f){fprintf(f,"[Hook] getSwapChainQueue: found via fallback map\n");fclose(f);} });
                return queue;
            }
        }
    }
    {
        static std::once_flag sNull;
        std::call_once(sNull, [] { FILE* f = nullptr; fopen_s(&f, "mods/playback/debug_log.txt", "a"); if(f){fprintf(f,"[Hook] getSwapChainQueue: returning NULL\n");fclose(f);} });
    }
    return nullptr;
}

ComPtr<ID3D12CommandQueue> getDeviceQueue(ID3D12Device* device) {
    if (!device) return nullptr;
    std::scoped_lock lock(gDeviceQueueMapMutex);
    auto const       it = gDeviceQueueMap.find(device);
    if (it == gDeviceQueueMap.end() || it->second.ambiguous) return nullptr;
    return it->second.queue;
}

void unbindSwapChainQueue(IDXGISwapChain* swapChain) {
    if (swapChain) {
        swapChain->SetPrivateDataInterface(SwapChainQueueGuid, nullptr);
        std::scoped_lock lk(gSwapChainQueueFallbackMutex);
        auto it = gSwapChainQueueFallback.find(swapChain);
        if (it != gSwapChainQueueFallback.end()) {
            if (it->second) it->second->Release();
            gSwapChainQueueFallback.erase(it);
        }
    }
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
    void*& outCreateSwapChainForComposition,
    void*& outCreateCommandQueue
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
        HRESULT const        hardwareResult = result;
        ComPtr<IDXGIAdapter> warpAdapter;
        result = factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter));
        if (FAILED(result)) {
            getLogger().error(
                "Unable to enumerate a WARP adapter for replay timeline hooks "
                "(hardware HRESULT=0x{:08X}, WARP HRESULT=0x{:08X})",
                static_cast<uint32_t>(hardwareResult),
                static_cast<uint32_t>(result)
            );
            return false;
        }
        result = D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (FAILED(result)) {
            getLogger().error(
                "Unable to create a D3D12 device for replay timeline hooks "
                "(hardware HRESULT=0x{:08X}, WARP HRESULT=0x{:08X})",
                static_cast<uint32_t>(hardwareResult),
                static_cast<uint32_t>(result)
            );
            return false;
        }
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
    outCreateCommandQueue            = getVtableEntry(device.Get(), DeviceCreateCommandQueueIndex);
    bool const resolved = outPresent && outPresent1 && outResizeBuffers && outResizeBuffers1 && outCreateSwapChain
                       && outCreateSwapChainForHwnd && outCreateSwapChainForCoreWindow
                       && outCreateSwapChainForComposition && outCreateCommandQueue;
    if (!resolved) getLogger().error("One or more replay timeline DXGI vtable targets resolved to null");
    else {
        FILE* f = nullptr; fopen_s(&f, "mods/playback/debug_log.txt", "a"); if(f){fprintf(f,"[Hook] resolveHookTargets SUCCESS (create=%p, hwnd=%p, core=%p, comp=%p)\n", outCreateSwapChain, outCreateSwapChainForHwnd, outCreateSwapChainForCoreWindow, outCreateSwapChainForComposition);fclose(f);}
    }
    return resolved;
}

bool hookRendererInit(bool enable) {
    std::scoped_lock lock(gRendererInitHookMutex);
    static bool      installed{};

    if (enable) {
        gRendererInitHookStopping.store(false, std::memory_order_release);
        if (installed) return true;
        if (RendererInitHook::hook() != 0) {
            gRendererInitHookStopping.store(true, std::memory_order_release);
            return false;
        }
        installed = true;
        return true;
    }

    gRendererInitHookStopping.store(true, std::memory_order_release);
    if (!installed) return waitForActiveRendererInitDetours();
    if (!RendererInitHook::unhook()) {
        gRendererInitHookStopping.store(false, std::memory_order_release);
        return false;
    }
    installed = false;
    return waitForActiveRendererInitDetours();
}

bool hookD3D12(bool enable) {
    std::scoped_lock lock(gTimelineHookMutex);
    auto&            state = hookState();

    if (enable) {
        if (coreInstalled(state)) {
            if (!captureInstalled(state)) (void)installCaptureHooks(state);
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
                state.targets.createSwapChainForComposition,
                state.targets.createCommandQueue
            )) {
            getLogger().error("Unable to resolve D3D12 hook targets for the replay ImGui timeline");
            return false;
        }
        bool const captureOk = installCaptureHooks(state);
        if (!captureOk) {
            getLogger().warn(
                "One or more replay queue capture hooks are unavailable; existing swap chains require a unique captured Direct queue"
            );
        }
        if (installCoreHooks(state)) {
            gTimelineHooksStopping.store(false, std::memory_order_release);
            FILE* file = nullptr;
            fopen_s(&file, "mods/playback/debug_log.txt", "a");
            if (file) {
                fprintf(
                    file,
                    "[Hook] enabled core=1 capture=%d present=%d present1=%d resize=%d resize1=%d queue=%d "
                    "create=%d/%d/%d/%d\n",
                    captureInstalled(state),
                    state.present,
                    state.present1,
                    state.resizeBuffers,
                    state.resizeBuffers1,
                    state.createCommandQueue,
                    state.createSwapChain,
                    state.createSwapChainForHwnd,
                    state.createSwapChainForCoreWindow,
                    state.createSwapChainForComposition
                );
                fclose(file);
            }
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

    if (!noneInstalled(state)) {
        gTimelineHooksStopping.store(true, std::memory_order_release);
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

} // namespace playback::editor::renderer
