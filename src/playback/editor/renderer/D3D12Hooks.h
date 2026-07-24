#pragma once

#include "playback/editor/renderer/D3D12Compat.h"

#include <Windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>

struct ID3D12CommandQueue;

namespace playback::editor::renderer {

using Microsoft::WRL::ComPtr;

inline constexpr UINT  SrvDescriptorCount  = 32;
inline constexpr DWORD GpuWaitTimeoutMs    = 2000;
inline constexpr DWORD DetourWaitTimeoutMs = 2000;
inline constexpr GUID  SwapChainQueueGuid{
    0xe185a345,
    0x1169,
    0x4fc8,
     {0xa4, 0x4b, 0x86, 0x73, 0xd1, 0x5d, 0x7b, 0x2f}
};

[[nodiscard]] bool isTimelineRenderingEnabled();

[[nodiscard]] bool hookD3D12(bool enable);

[[nodiscard]] bool hookRendererInit(bool enable);

bool waitForActiveDetours();

bool getDirectCommandQueue(IUnknown* object, ComPtr<ID3D12CommandQueue>& queue);

void bindSwapChainQueue(IDXGISwapChain* swapChain, IUnknown* queueObject);

ComPtr<ID3D12CommandQueue> getSwapChainQueue(IDXGISwapChain* swapChain);

void unbindSwapChainQueue(IDXGISwapChain* swapChain);

ComPtr<ID3D12CommandQueue> getResizePresentQueue(UINT bufferCount, IUnknown* const* presentQueues);

[[nodiscard]] bool resolveHookTargets(
    void*& outPresent,
    void*& outPresent1,
    void*& outResizeBuffers,
    void*& outResizeBuffers1,
    void*& outCreateSwapChain,
    void*& outCreateSwapChainForHwnd,
    void*& outCreateSwapChainForCoreWindow,
    void*& outCreateSwapChainForComposition
);

} // namespace playback::editor::renderer
