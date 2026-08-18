#include "D3D12FrameTapBackend.h"

#include "playback/editor/graphics/D3D12Compat.h"

#include "playback/Playback.h"

#include <Windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

namespace playback::editor::graphics {

using Microsoft::WRL::ComPtr;
using visuals::CapturedFrame;
using visuals::FramePixelFormat;
using visuals::FrameTapBackendCapture;
using visuals::FrameTapError;

namespace {

constexpr DWORD ReadbackWaitTimeoutMs = 10000;

bool isSupported(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM;
}

FramePixelFormat pixelFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_B8G8R8A8_UNORM ? FramePixelFormat::Bgra8 : FramePixelFormat::Rgba8;
}

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

bool shouldLogCapture(FrameTapBackendCapture const& capture) {
    return capture.ticket.frameIndex < 2 || capture.ticket.frameIndex % 60 == 0;
}

} // namespace

struct D3D12FrameTapBackend::Impl {
    enum class SlotState : uint8_t { Free, AwaitingFence, Submitted, Processing, Retired };

    struct Slot {
        ComPtr<ID3D12Device>                  device;
        ComPtr<ID3D12CommandAllocator>        commandAllocator;
        ComPtr<ID3D12GraphicsCommandList>     commandList;
        ComPtr<ID3D12Resource>                readback;
        ComPtr<ID3D12Resource>                exportTexture;
        ComPtr<ID3D12Fence>                   fence;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT    footprint{};
        uint64_t                              byteCount{};
        uint64_t                              fenceValue{};
        uint32_t                              width{};
        uint32_t                              height{};
        DXGI_FORMAT                           format{DXGI_FORMAT_UNKNOWN};
        bool                                  commandListClosed{};
        SlotState                             state{SlotState::Free};
        std::optional<FrameTapBackendCapture> capture;
    };

    explicit Impl(visuals::FrameTap& tap) : frameTap(tap) {}

    ~Impl() { reset(FrameTapError::Cancelled, "D3D12 frame capture stopped"); }

    visuals::FrameTap&      frameTap;
    std::mutex              mutex;
    std::condition_variable changed;
    std::vector<Slot>       slots;
    std::optional<size_t>   pendingSubmission;
    ComPtr<ID3D12Device>    submissionDevice;
    ComPtr<ID3D12Fence>     submissionFence;
    uint64_t                nextSubmissionFenceValue{};
    std::thread             worker;
    HANDLE                  fenceEvent{};
    HANDLE                  stopEvent{};
    bool                    stopping{};

    bool startWorker() {
        if (worker.joinable()) return true;
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        stopEvent  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!fenceEvent || !stopEvent) {
            if (fenceEvent) CloseHandle(fenceEvent);
            if (stopEvent) CloseHandle(stopEvent);
            fenceEvent = nullptr;
            stopEvent  = nullptr;
            return false;
        }
        stopping = false;
        worker   = std::thread([this] { workerLoop(); });
        return true;
    }

    bool prepareSlot(Slot& slot, ID3D12Device* device, D3D12_RESOURCE_DESC const& sourceDesc) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        uint64_t                           totalBytes{};
        device->GetCopyableFootprints(&sourceDesc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);
        if (totalBytes == 0) {
            getLogger().error(
                "D3D12 frame readback footprint is empty (size={}x{}, format={}, samples={})",
                sourceDesc.Width,
                sourceDesc.Height,
                static_cast<uint32_t>(sourceDesc.Format),
                sourceDesc.SampleDesc.Count
            );
            return false;
        }
        bool const reusable = slot.device.Get() == device && slot.readback && slot.width == sourceDesc.Width
                           && slot.height == sourceDesc.Height && slot.format == sourceDesc.Format
                           && slot.byteCount == totalBytes;
        if (!reusable) {
            slot.device.Reset();
            slot.commandAllocator.Reset();
            slot.commandList.Reset();
            slot.commandListClosed = false;
            slot.readback.Reset();
            slot.exportTexture.Reset();
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type                 = D3D12_HEAP_TYPE_READBACK;
            heap.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            heap.CreationNodeMask     = 1;
            heap.VisibleNodeMask      = 1;

            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width            = totalBytes;
            buffer.Height           = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels        = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> readback;
            HRESULT const          createResult = device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&readback)
            );
            if (FAILED(createResult)) {
                getLogger().error(
                    "D3D12 frame readback allocation failed (hr=0x{:08X}, bytes={}, size={}x{}, format={})",
                    static_cast<uint32_t>(createResult),
                    totalBytes,
                    sourceDesc.Width,
                    sourceDesc.Height,
                    static_cast<uint32_t>(sourceDesc.Format)
                );
                return false;
            }
            slot.device   = device;
            slot.readback = std::move(readback);
        }
        slot.footprint = footprint;
        slot.byteCount = totalBytes;
        slot.width     = static_cast<uint32_t>(sourceDesc.Width);
        slot.height    = sourceDesc.Height;
        slot.format    = sourceDesc.Format;
        return true;
    }

    bool prepareSubmittedSlot(Slot& slot, ID3D12Device* device, D3D12_RESOURCE_DESC const& sourceDesc) {
        auto copyDesc               = sourceDesc;
        copyDesc.Alignment          = 0;
        copyDesc.SampleDesc.Count   = 1;
        copyDesc.SampleDesc.Quality = 0;
        if (!prepareSlot(slot, device, copyDesc)) return false;

        if (!slot.commandAllocator) {
            HRESULT const createResult =
                device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.commandAllocator));
            if (FAILED(createResult)) {
                getLogger().error(
                    "D3D12 submitted frame command allocator creation failed (hr=0x{:08X})",
                    static_cast<uint32_t>(createResult)
                );
                return false;
            }
        }
        if (!slot.commandList) {
            HRESULT const createResult = device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                slot.commandAllocator.Get(),
                nullptr,
                IID_PPV_ARGS(&slot.commandList)
            );
            if (FAILED(createResult)) {
                getLogger().error(
                    "D3D12 submitted frame command list creation failed (hr=0x{:08X})",
                    static_cast<uint32_t>(createResult)
                );
                return false;
            }
        }
        if (slot.commandList && !slot.commandListClosed) {
            HRESULT const closeResult = slot.commandList->Close();
            if (FAILED(closeResult)) {
                getLogger().error(
                    "D3D12 submitted frame command list initial close failed (hr=0x{:08X}, device=0x{:08X})",
                    static_cast<uint32_t>(closeResult),
                    static_cast<uint32_t>(device->GetDeviceRemovedReason())
                );
                return false;
            }
            slot.commandListClosed = true;
        }

        auto const resolved = slot.exportTexture ? slot.exportTexture->GetDesc() : D3D12_RESOURCE_DESC{};
        bool const reusable = slot.exportTexture && resolved.Width == sourceDesc.Width
                           && resolved.Height == sourceDesc.Height && resolved.Format == sourceDesc.Format
                           && resolved.SampleDesc.Count == 1;
        if (!reusable) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type                 = D3D12_HEAP_TYPE_DEFAULT;
            heap.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            heap.CreationNodeMask     = 1;
            heap.VisibleNodeMask      = 1;

            D3D12_RESOURCE_DESC resolvedDesc = sourceDesc;
            resolvedDesc.Alignment           = 0;
            resolvedDesc.SampleDesc.Count    = 1;
            resolvedDesc.SampleDesc.Quality  = 0;
            resolvedDesc.Flags               = D3D12_RESOURCE_FLAG_NONE;
            HRESULT const createResult       = device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &resolvedDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                IID_PPV_ARGS(&slot.exportTexture)
            );
            if (FAILED(createResult)) {
                getLogger().error(
                    "D3D12 export texture allocation failed (hr=0x{:08X}, size={}x{}, format={}, sourceSamples={})",
                    static_cast<uint32_t>(createResult),
                    sourceDesc.Width,
                    sourceDesc.Height,
                    static_cast<uint32_t>(sourceDesc.Format),
                    sourceDesc.SampleDesc.Count
                );
                return false;
            }
        }
        return true;
    }

    bool ensureSubmissionFence(ID3D12Device* device) {
        if (submissionFence && submissionDevice.Get() == device) return true;
        if (std::ranges::any_of(slots, [](Slot const& slot) { return slot.state != SlotState::Free; })) return false;
        ComPtr<ID3D12Fence> fence;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        submissionDevice         = device;
        submissionFence          = std::move(fence);
        nextSubmissionFenceValue = 0;
        return true;
    }

    void workerLoop() {
        try {
            workerLoopBody();
        } catch (std::exception const& error) {
            getLogger().error("D3D12 frame tap worker failed: {}", error.what());
            frameTap.failActive(FrameTapError::BackendUnavailable, "The D3D12 frame tap worker failed");
            std::scoped_lock lock(mutex);
            stopping = true;
            if (stopEvent) SetEvent(stopEvent);
            changed.notify_all();
        } catch (...) {
            getLogger().error("D3D12 frame tap worker failed with an unknown exception");
            frameTap.failActive(FrameTapError::BackendUnavailable, "The D3D12 frame tap worker failed");
            std::scoped_lock lock(mutex);
            stopping = true;
            if (stopEvent) SetEvent(stopEvent);
            changed.notify_all();
        }
    }

    void workerLoopBody() {
        while (true) {
            size_t                             slotIndex{};
            ComPtr<ID3D12Resource>             readback;
            ComPtr<ID3D12Fence>                fence;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            FrameTapBackendCapture             capture;
            uint64_t                           byteCount{};
            uint64_t                           fenceValue{};
            uint32_t                           width{};
            uint32_t                           height{};
            DXGI_FORMAT                        format{DXGI_FORMAT_UNKNOWN};
            ComPtr<ID3D12Device>               device;

            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [this] {
                    return stopping || std::ranges::any_of(slots, [](Slot const& slot) {
                               return slot.state == SlotState::Submitted;
                           });
                });
                if (stopping) return;
                auto selected = slots.end();
                for (auto it = slots.begin(); it != slots.end(); ++it) {
                    if (it->state != SlotState::Submitted || !it->capture) continue;
                    if (selected == slots.end() || it->capture->captureId < selected->capture->captureId) selected = it;
                }
                if (selected == slots.end()) continue;
                slotIndex       = static_cast<size_t>(std::distance(slots.begin(), selected));
                selected->state = SlotState::Processing;
                readback        = selected->readback;
                device          = selected->device;
                fence           = selected->fence;
                footprint       = selected->footprint;
                capture         = *selected->capture;
                byteCount       = selected->byteCount;
                fenceValue      = selected->fenceValue;
                width           = selected->width;
                height          = selected->height;
                format          = selected->format;
            }

            bool reusable = true;
            if (!fence || fenceValue == 0 || !readback) {
                frameTap.fail(capture, FrameTapError::FenceFailed, "D3D12 frame submission has no fence");
                reusable = false;
            } else if (fence->GetCompletedValue() < fenceValue) {
                if (FAILED(fence->SetEventOnCompletion(fenceValue, fenceEvent))) {
                    frameTap.fail(capture, FrameTapError::FenceFailed, "Unable to wait for a D3D12 frame fence");
                    reusable = false;
                } else {
                    HANDLE const events[]{fenceEvent, stopEvent};
                    DWORD const  wait = WaitForMultipleObjects(2, events, FALSE, ReadbackWaitTimeoutMs);
                    if (wait == WAIT_OBJECT_0 + 1) return;
                    if (wait != WAIT_OBJECT_0) {
                        frameTap.fail(capture, FrameTapError::FenceFailed, "Timed out waiting for a D3D12 frame fence");
                        reusable = false;
                    }
                }
            }

            if (reusable) {
                uint64_t const packedRowBytes = static_cast<uint64_t>(width) * 4ull;
                uint64_t       lastByte       = footprint.Offset;
                bool           validLayout =
                    footprint.Footprint.RowPitch >= packedRowBytes && height != 0
                    && (height - 1) <= (std::numeric_limits<uint64_t>::max() - lastByte) / footprint.Footprint.RowPitch;
                if (validLayout) {
                    lastByte    += static_cast<uint64_t>(height - 1) * footprint.Footprint.RowPitch;
                    validLayout  = packedRowBytes <= std::numeric_limits<uint64_t>::max() - lastByte
                                && lastByte + packedRowBytes <= byteCount;
                }
                if (!validLayout) {
                    frameTap.fail(capture, FrameTapError::MapFailed, "D3D12 readback footprint is invalid");
                    reusable = false;
                }
            }

            if (reusable) {
                void*         mapped{};
                D3D12_RANGE   readRange{0, static_cast<SIZE_T>(byteCount)};
                HRESULT const mapResult = readback->Map(0, &readRange, &mapped);
                if (FAILED(mapResult)) {
                    HRESULT const deviceReason = device ? device->GetDeviceRemovedReason() : E_POINTER;
                    auto const    error = FAILED(deviceReason) ? FrameTapError::DeviceLost : FrameTapError::MapFailed;
                    getLogger().error(
                        "D3D12 readback Map failed (capture={}, frame={}, map=0x{:08X}, device=0x{:08X}, "
                        "resource=0x{:X}, fence={}, size={}x{}, rowPitch={}, bytes={})",
                        capture.captureId,
                        capture.ticket.frameIndex,
                        static_cast<uint32_t>(mapResult),
                        static_cast<uint32_t>(deviceReason),
                        reinterpret_cast<uintptr_t>(readback.Get()),
                        fenceValue,
                        width,
                        height,
                        footprint.Footprint.RowPitch,
                        byteCount
                    );
                    frameTap.fail(
                        capture,
                        error,
                        std::format(
                            "Unable to map a completed D3D12 frame (HRESULT=0x{:08X}, device=0x{:08X})",
                            static_cast<uint32_t>(mapResult),
                            static_cast<uint32_t>(deviceReason)
                        )
                    );
                    reusable = false;
                } else {
                    CapturedFrame frame;
                    frame.width            = width;
                    frame.height           = height;
                    frame.rowPitch         = width * 4;
                    frame.pixelFormat      = pixelFormat(format);
                    auto const packedBytes = static_cast<uint64_t>(frame.rowPitch) * height;
                    if (packedBytes > std::numeric_limits<size_t>::max()) {
                        frameTap.fail(capture, FrameTapError::MapFailed, "D3D12 captured frame is too large");
                        reusable = false;
                    } else {
                        frame.pixels.resize(static_cast<size_t>(packedBytes));
                        for (uint32_t y = 0; y < height; ++y) {
                            auto const* source = static_cast<std::byte const*>(mapped) + footprint.Offset
                                               + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
                            auto*       target = frame.pixels.data() + static_cast<size_t>(y) * frame.rowPitch;
                            std::memcpy(target, source, frame.rowPitch);
                        }
                        if (shouldLogCapture(capture)) {
                            getLogger().debug(
                                "D3D12 frame capture completed (capture={}, frame={}, size={}x{}, rowPitch={}, "
                                "source={}x{}, samples={}, resource=0x{:X}, fence={})",
                                capture.captureId,
                                capture.ticket.frameIndex,
                                frame.width,
                                frame.height,
                                footprint.Footprint.RowPitch,
                                capture.submission.width,
                                capture.submission.height,
                                capture.submission.sampleCount,
                                reinterpret_cast<uintptr_t>(capture.submission.exportResource),
                                fenceValue
                            );
                        }
                        frameTap.complete(capture, std::move(frame));
                    }
                    D3D12_RANGE const writtenRange{0, 0};
                    readback->Unmap(0, &writtenRange);
                }
            }

            {
                std::scoped_lock lock(mutex);
                if (slotIndex < slots.size() && slots[slotIndex].capture
                    && slots[slotIndex].capture->captureId == capture.captureId) {
                    auto& slot = slots[slotIndex];
                    slot.capture.reset();
                    slot.fence.Reset();
                    slot.fenceValue = 0;
                    slot.state      = reusable ? SlotState::Free : SlotState::Retired;
                }
            }
        }
    }

    void reset(FrameTapError error, std::string message) {
        frameTap.failActive(error, std::move(message));
        {
            std::scoped_lock lock(mutex);
            stopping = true;
            if (stopEvent) SetEvent(stopEvent);
        }
        changed.notify_all();
        if (worker.joinable()) worker.join();
        {
            std::scoped_lock lock(mutex);
            slots.clear();
            pendingSubmission.reset();
            submissionDevice.Reset();
            submissionFence.Reset();
            nextSubmissionFenceValue = 0;
            stopping                 = false;
            if (fenceEvent) CloseHandle(fenceEvent);
            if (stopEvent) CloseHandle(stopEvent);
            fenceEvent = nullptr;
            stopEvent  = nullptr;
        }
    }
};

D3D12FrameTapBackend::D3D12FrameTapBackend(visuals::FrameTap& frameTap) : mImpl(std::make_unique<Impl>(frameTap)) {}

D3D12FrameTapBackend::~D3D12FrameTapBackend() = default;

bool D3D12FrameTapBackend::capture(
    ID3D12Device*              device,
    ID3D12CommandQueue*        queue,
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource*            source,
    uint32_t                   sourceState
) {
    if (!device || !queue || !commandList || !source || !mImpl->frameTap.requiresRenderPass()) return false;
    ComPtr<ID3D12Device> commandListDevice;
    ComPtr<ID3D12Device> queueDevice;
    ComPtr<ID3D12Device> sourceDevice;
    if (FAILED(commandList->GetDevice(IID_PPV_ARGS(&commandListDevice))) || commandListDevice.Get() != device
        || FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || queueDevice.Get() != device) {
        mImpl->frameTap.failActive(
            FrameTapError::BackendUnavailable,
            "The D3D12 frame command list and queue belong to a different device"
        );
        return false;
    }
    if (FAILED(source->GetDevice(IID_PPV_ARGS(&sourceDevice))) || sourceDevice.Get() != device) {
        mImpl->frameTap.failActive(
            FrameTapError::BackendUnavailable,
            "The D3D12 frame source belongs to a different device"
        );
        return false;
    }
    auto const sourceDesc = source->GetDesc();
    if (sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sourceDesc.SampleDesc.Count != 1
        || sourceDesc.Width == 0 || sourceDesc.Height == 0 || !isSupported(sourceDesc.Format)) {
        mImpl->frameTap.failActive(FrameTapError::UnsupportedFormat, "Unsupported D3D12 frame format");
        return false;
    }

    std::scoped_lock lock(mImpl->mutex);
    if (!mImpl->startWorker() || mImpl->pendingSubmission) {
        if (!mImpl->worker.joinable()) {
            mImpl->frameTap.failActive(FrameTapError::BackendUnavailable, "Unable to start the D3D12 frame worker");
        }
        return false;
    }
    auto const capacity = mImpl->frameTap.captureCapacity();
    if (mImpl->slots.size() < capacity) mImpl->slots.resize(capacity);
    auto slot = std::find_if(mImpl->slots.begin(), mImpl->slots.end(), [](Impl::Slot const& value) {
        return value.state == Impl::SlotState::Free;
    });
    if (slot == mImpl->slots.end()) {
        for (auto& candidate : mImpl->slots) {
            if (candidate.state != Impl::SlotState::Retired || candidate.capture) continue;
            candidate.device.Reset();
            candidate.readback.Reset();
            candidate.fence.Reset();
            candidate.fenceValue = 0;
            candidate.state      = Impl::SlotState::Free;
        }
        slot = std::find_if(mImpl->slots.begin(), mImpl->slots.end(), [](Impl::Slot const& value) {
            return value.state == Impl::SlotState::Free;
        });
    }
    if (slot == mImpl->slots.end()) return false;
    if (!mImpl->prepareSlot(*slot, device, sourceDesc)) {
        mImpl->frameTap.failActive(
            FrameTapError::BackendUnavailable,
            "Unable to allocate D3D12 frame readback resources"
        );
        return false;
    }
    auto capture = mImpl->frameTap.beginCapture();
    if (!capture) return false;

    capture->submission = {
        static_cast<uint32_t>(sourceDesc.Width),
        sourceDesc.Height,
        sourceDesc.SampleDesc.Count,
        pixelFormat(sourceDesc.Format),
        source,
        queue,
        nullptr,
        0,
        sourceState,
    };

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource       = slot->readback.Get();
    destination.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = slot->footprint;
    D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
    sourceLocation.pResource        = source;
    sourceLocation.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sourceLocation.SubresourceIndex = 0;
    commandList->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);

    slot->capture            = *capture;
    slot->state              = Impl::SlotState::AwaitingFence;
    auto const slotIndex     = static_cast<size_t>(std::distance(mImpl->slots.begin(), slot));
    mImpl->pendingSubmission = slotIndex;
    if (shouldLogCapture(*capture)) {
        getLogger().debug(
            "D3D12 frame capture bound (capture={}, frame={}, resource=0x{:X}, slot={}, size={}x{}, format={}, "
            "samples={}, sourceState=0x{:X})",
            capture->captureId,
            capture->ticket.frameIndex,
            reinterpret_cast<uintptr_t>(source),
            slotIndex,
            sourceDesc.Width,
            sourceDesc.Height,
            static_cast<uint32_t>(sourceDesc.Format),
            sourceDesc.SampleDesc.Count,
            sourceState
        );
    }
    return true;
}

bool D3D12FrameTapBackend::captureSubmitted(
    ID3D12Device*       device,
    ID3D12CommandQueue* queue,
    ID3D12Resource*     source,
    uint32_t            sourceState
) {
    if (!device || !queue || !source || !mImpl->frameTap.requiresRenderPass()) return false;

    ComPtr<ID3D12Device> sourceDevice;
    ComPtr<ID3D12Device> queueDevice;
    if (FAILED(source->GetDevice(IID_PPV_ARGS(&sourceDevice))) || sourceDevice.Get() != device
        || FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || queueDevice.Get() != device) {
        mImpl->frameTap.failActive(
            FrameTapError::BackendUnavailable,
            "The D3D12 submitted frame does not belong to the renderer device"
        );
        return false;
    }

    auto const sourceDesc = source->GetDesc();
    if (sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sourceDesc.Width == 0 || sourceDesc.Height == 0
        || (sourceDesc.SampleDesc.Count != 1 && sourceDesc.SampleDesc.Count != 2 && sourceDesc.SampleDesc.Count != 4
            && sourceDesc.SampleDesc.Count != 8)
        || !isSupported(sourceDesc.Format)) {
        mImpl->frameTap.failActive(FrameTapError::UnsupportedFormat, "Unsupported D3D12 submitted frame format");
        return false;
    }

    std::scoped_lock lock(mImpl->mutex);
    if (!mImpl->startWorker() || mImpl->pendingSubmission || !mImpl->ensureSubmissionFence(device)) {
        if (!mImpl->worker.joinable()) {
            mImpl->frameTap.failActive(FrameTapError::BackendUnavailable, "Unable to start the D3D12 frame worker");
        }
        return false;
    }

    auto const capacity = mImpl->frameTap.captureCapacity();
    if (mImpl->slots.size() < capacity) mImpl->slots.resize(capacity);
    auto findFreeSlot = [&] {
        return std::find_if(mImpl->slots.begin(), mImpl->slots.end(), [](Impl::Slot const& value) {
            return value.state == Impl::SlotState::Free;
        });
    };
    auto slot = findFreeSlot();
    if (slot == mImpl->slots.end()) {
        for (auto& candidate : mImpl->slots) {
            if (candidate.state != Impl::SlotState::Retired || candidate.capture) continue;
            candidate.device.Reset();
            candidate.commandAllocator.Reset();
            candidate.commandList.Reset();
            candidate.commandListClosed = false;
            candidate.readback.Reset();
            candidate.exportTexture.Reset();
            candidate.fence.Reset();
            candidate.fenceValue = 0;
            candidate.state      = Impl::SlotState::Free;
        }
        slot = findFreeSlot();
    }
    if (slot == mImpl->slots.end()) return false;
    if (!mImpl->prepareSubmittedSlot(*slot, device, sourceDesc)) {
        mImpl->frameTap.failActive(
            FrameTapError::BackendUnavailable,
            "Unable to allocate D3D12 submitted frame resources"
        );
        return false;
    }

    auto capture = mImpl->frameTap.beginCapture();
    if (!capture) return false;

    capture->submission = {
        static_cast<uint32_t>(sourceDesc.Width),
        sourceDesc.Height,
        sourceDesc.SampleDesc.Count,
        pixelFormat(sourceDesc.Format),
        source,
        queue,
        nullptr,
        0,
        sourceState,
    };
    size_t const slotIndex = static_cast<size_t>(std::distance(mImpl->slots.begin(), slot));

    auto failSubmission = [&](FrameTapError error, std::string message) {
        slot->capture.reset();
        slot->state = Impl::SlotState::Retired;
        mImpl->frameTap.fail(*capture, error, std::move(message));
    };

    HRESULT const allocatorResetResult   = slot->commandAllocator->Reset();
    HRESULT const commandListResetResult = SUCCEEDED(allocatorResetResult)
                                             ? slot->commandList->Reset(slot->commandAllocator.Get(), nullptr)
                                             : allocatorResetResult;
    if (FAILED(allocatorResetResult) || FAILED(commandListResetResult)) {
        getLogger().error(
            "D3D12 submitted frame command list reset failed (allocator=0x{:08X}, list=0x{:08X}, device=0x{:08X})",
            static_cast<uint32_t>(allocatorResetResult),
            static_cast<uint32_t>(commandListResetResult),
            static_cast<uint32_t>(device->GetDeviceRemovedReason())
        );
        failSubmission(FrameTapError::BackendUnavailable, "Unable to reset the D3D12 submitted frame command list");
        return false;
    }
    slot->commandListClosed = false;

    auto* const exportTexture = slot->exportTexture.Get();
    if (!exportTexture) {
        mImpl->frameTap.fail(*capture, FrameTapError::BackendUnavailable, "The D3D12 export texture is unavailable");
        slot->state = Impl::SlotState::Retired;
        return false;
    }
    auto const                            sourceStateEnum = static_cast<D3D12_RESOURCE_STATES>(sourceState);
    std::array<D3D12_RESOURCE_BARRIER, 6> barriers{};
    UINT                                  barrierCount{};
    auto addBarrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        if (before == after) return;
        auto& barrier                  = barriers[barrierCount++];
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter  = after;
    };

    if (sourceDesc.SampleDesc.Count > 1) {
        addBarrier(source, sourceStateEnum, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        addBarrier(exportTexture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        slot->commandList->ResourceBarrier(barrierCount, barriers.data());
        barrierCount = 0;
        slot->commandList->ResolveSubresource(exportTexture, 0, source, 0, sourceDesc.Format);
        addBarrier(exportTexture, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        slot->commandList->ResourceBarrier(1, &barriers[0]);
    } else {
        addBarrier(source, sourceStateEnum, D3D12_RESOURCE_STATE_COPY_SOURCE);
        addBarrier(exportTexture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        slot->commandList->ResourceBarrier(barrierCount, barriers.data());
        barrierCount = 0;
        slot->commandList->CopyResource(exportTexture, source);
        addBarrier(exportTexture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        slot->commandList->ResourceBarrier(1, &barriers[0]);
    }

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource       = slot->readback.Get();
    destination.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = slot->footprint;
    D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
    sourceLocation.pResource        = exportTexture;
    sourceLocation.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sourceLocation.SubresourceIndex = 0;
    slot->commandList->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);

    barrierCount = 0;
    addBarrier(exportTexture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (sourceDesc.SampleDesc.Count > 1) {
        addBarrier(source, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, sourceStateEnum);
    } else {
        addBarrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceStateEnum);
    }
    slot->commandList->ResourceBarrier(barrierCount, barriers.data());
    HRESULT const closeResult = slot->commandList->Close();
    if (FAILED(closeResult)) {
        getLogger().error(
            "D3D12 submitted frame command list close failed (hr=0x{:08X}, device=0x{:08X}, sourceState=0x{:X}, "
            "samples={}, barriers={})",
            static_cast<uint32_t>(closeResult),
            static_cast<uint32_t>(device->GetDeviceRemovedReason()),
            sourceState,
            sourceDesc.SampleDesc.Count,
            barrierCount
        );
        failSubmission(FrameTapError::BackendUnavailable, "Unable to close the D3D12 submitted frame command list");
        return false;
    }
    slot->commandListClosed = true;

    slot->state         = Impl::SlotState::Submitted;
    slot->fence         = mImpl->submissionFence;
    slot->fenceValue    = ++mImpl->nextSubmissionFenceValue;
    capture->submission = {
        static_cast<uint32_t>(sourceDesc.Width),
        sourceDesc.Height,
        sourceDesc.SampleDesc.Count,
        pixelFormat(sourceDesc.Format),
        exportTexture,
        queue,
        mImpl->submissionFence.Get(),
        slot->fenceValue,
        sourceState,
    };
    if (shouldLogCapture(*capture)) {
        getLogger().debug(
            "D3D12 submitted frame capture queued (capture={}, frame={}, source=0x{:X}, export=0x{:X}, slot={}, "
            "size={}x{}, samples={}, format={}, sourceState=0x{:X}, fence={})",
            capture->captureId,
            capture->ticket.frameIndex,
            reinterpret_cast<uintptr_t>(source),
            reinterpret_cast<uintptr_t>(exportTexture),
            slotIndex,
            sourceDesc.Width,
            sourceDesc.Height,
            sourceDesc.SampleDesc.Count,
            static_cast<uint32_t>(sourceDesc.Format),
            sourceState,
            slot->fenceValue
        );
    }
    slot->capture = *capture;
    ID3D12CommandList* commandLists[]{slot->commandList.Get()};
    queue->ExecuteCommandLists(1, commandLists);
    if (FAILED(queue->Signal(mImpl->submissionFence.Get(), slot->fenceValue))) {
        failSubmission(FrameTapError::FenceFailed, "Unable to signal the D3D12 submitted frame fence");
        return false;
    }
    mImpl->changed.notify_one();
    return true;
}

void D3D12FrameTapBackend::submitted(ID3D12Fence* fence, uint64_t fenceValue) {
    if (!fence || fenceValue == 0) {
        submissionFailed(FrameTapError::FenceFailed, "D3D12 frame submission has no fence");
        return;
    }
    {
        std::scoped_lock lock(mImpl->mutex);
        if (!mImpl->pendingSubmission || *mImpl->pendingSubmission >= mImpl->slots.size()) return;
        auto& slot = mImpl->slots[*mImpl->pendingSubmission];
        if (slot.capture) {
            slot.capture->submission.completionFence      = fence;
            slot.capture->submission.completionFenceValue = fenceValue;
        }
        slot.fence      = fence;
        slot.fenceValue = fenceValue;
        slot.state      = Impl::SlotState::Submitted;
        mImpl->pendingSubmission.reset();
    }
    mImpl->changed.notify_one();
}

void D3D12FrameTapBackend::submissionFailed(FrameTapError error, std::string message) {
    std::optional<FrameTapBackendCapture> capture;
    {
        std::scoped_lock lock(mImpl->mutex);
        if (!mImpl->pendingSubmission || *mImpl->pendingSubmission >= mImpl->slots.size()) return;
        auto& slot = mImpl->slots[*mImpl->pendingSubmission];
        capture    = slot.capture;
        slot.capture.reset();
        slot.state = Impl::SlotState::Retired;
        mImpl->pendingSubmission.reset();
    }
    if (capture) {
        getLogger().error(
            "D3D12 frame capture submission failed (capture {}, frame {}, error {}): {}",
            capture->captureId,
            capture->ticket.frameIndex,
            static_cast<int>(error),
            message
        );
        mImpl->frameTap.fail(*capture, error, std::move(message));
    }
}

void D3D12FrameTapBackend::reset(FrameTapError error, std::string message) { mImpl->reset(error, std::move(message)); }

} // namespace playback::editor::graphics
