#include "D3D12FrameTapBackend.h"

#include "playback/editor/renderer/D3D12Compat.h"

#include <Windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace playback::editor::renderer {

using functions::render::CapturedFrame;
using functions::render::FramePixelFormat;
using functions::render::FrameTapBackendCapture;
using functions::render::FrameTapError;
using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD ReadbackWaitTimeoutMs = 10000;

bool isSupported(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM;
}

FramePixelFormat pixelFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_B8G8R8A8_UNORM ? FramePixelFormat::Bgra8 : FramePixelFormat::Rgba8;
}

} // namespace

struct D3D12FrameTapBackend::Impl {
    enum class SlotState : uint8_t { Free, AwaitingFence, Submitted, Processing, Retired };

    struct Slot {
        ComPtr<ID3D12Resource>                readback;
        ComPtr<ID3D12Fence>                   fence;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT    footprint{};
        uint64_t                              byteCount{};
        uint64_t                              fenceValue{};
        uint32_t                              width{};
        uint32_t                              height{};
        DXGI_FORMAT                           format{DXGI_FORMAT_UNKNOWN};
        SlotState                             state{SlotState::Free};
        std::optional<FrameTapBackendCapture> capture;
    };

    explicit Impl(functions::render::FrameTap& tap) : frameTap(tap) {}

    ~Impl() { reset(FrameTapError::Cancelled, "D3D12 frame capture stopped"); }

    functions::render::FrameTap& frameTap;
    std::mutex                   mutex;
    std::condition_variable      changed;
    std::vector<Slot>            slots;
    std::optional<size_t>        pendingSubmission;
    std::thread                  worker;
    HANDLE                       fenceEvent{};
    HANDLE                       stopEvent{};
    bool                         stopping{};

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
        if (totalBytes == 0) return false;
        bool const reusable = slot.readback && slot.width == sourceDesc.Width && slot.height == sourceDesc.Height
                           && slot.format == sourceDesc.Format && slot.byteCount == totalBytes;
        if (!reusable) {
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
            if (FAILED(device->CreateCommittedResource(
                    &heap,
                    D3D12_HEAP_FLAG_NONE,
                    &buffer,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(&readback)
                ))) {
                return false;
            }
            slot.readback = std::move(readback);
        }
        slot.footprint = footprint;
        slot.byteCount = totalBytes;
        slot.width     = static_cast<uint32_t>(sourceDesc.Width);
        slot.height    = sourceDesc.Height;
        slot.format    = sourceDesc.Format;
        return true;
    }

    void workerLoop() {
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
                void*       mapped{};
                D3D12_RANGE readRange{0, static_cast<SIZE_T>(byteCount)};
                if (FAILED(readback->Map(0, &readRange, &mapped))) {
                    frameTap.fail(capture, FrameTapError::MapFailed, "Unable to map a completed D3D12 frame");
                } else {
                    CapturedFrame frame;
                    frame.width            = width;
                    frame.height           = height;
                    frame.rowPitch         = width * 4;
                    frame.pixelFormat      = pixelFormat(format);
                    auto const packedBytes = static_cast<uint64_t>(frame.rowPitch) * height;
                    if (packedBytes > std::numeric_limits<size_t>::max()) {
                        frameTap.fail(capture, FrameTapError::MapFailed, "D3D12 captured frame is too large");
                    } else {
                        frame.pixels.resize(static_cast<size_t>(packedBytes));
                        for (uint32_t y = 0; y < height; ++y) {
                            auto const* source = static_cast<std::byte const*>(mapped) + footprint.Offset
                                               + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
                            auto* target = frame.pixels.data() + static_cast<size_t>(y) * frame.rowPitch;
                            std::memcpy(target, source, frame.rowPitch);
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
            stopping = false;
            if (fenceEvent) CloseHandle(fenceEvent);
            if (stopEvent) CloseHandle(stopEvent);
            fenceEvent = nullptr;
            stopEvent  = nullptr;
        }
    }
};

D3D12FrameTapBackend::D3D12FrameTapBackend(functions::render::FrameTap& frameTap)
: mImpl(std::make_unique<Impl>(frameTap)) {}

D3D12FrameTapBackend::~D3D12FrameTapBackend() = default;

bool D3D12FrameTapBackend::capture(
    ID3D12Device*              device,
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource*            source
) {
    if (!device || !commandList || !source || !mImpl->frameTap.requiresRenderPass()) return false;
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
    mImpl->pendingSubmission = static_cast<size_t>(std::distance(mImpl->slots.begin(), slot));
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
        auto& slot      = mImpl->slots[*mImpl->pendingSubmission];
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
    if (capture) mImpl->frameTap.fail(*capture, error, std::move(message));
}

void D3D12FrameTapBackend::reset(FrameTapError error, std::string message) { mImpl->reset(error, std::move(message)); }

} // namespace playback::editor::renderer
