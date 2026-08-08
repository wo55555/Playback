#include "D3D11FrameTapBackend.h"

#include "playback/Playback.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace playback::editor::renderer {

using functions::render::CapturedFrame;
using functions::render::FramePixelFormat;
using functions::render::FrameTapBackendCapture;
using functions::render::FrameTapError;
using Microsoft::WRL::ComPtr;

namespace {

bool isSupported(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM;
}

FramePixelFormat pixelFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_B8G8R8A8_UNORM ? FramePixelFormat::Bgra8 : FramePixelFormat::Rgba8;
}

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

} // namespace

struct D3D11FrameTapBackend::Impl {
    struct Slot {
        ComPtr<ID3D11Device>                  device;
        ComPtr<ID3D11Texture2D>               staging;
        ComPtr<ID3D11Query>                   completionQuery;
        D3D11_TEXTURE2D_DESC                  sourceDesc{};
        std::optional<FrameTapBackendCapture> capture;
    };

    explicit Impl(functions::render::FrameTap& tap) : frameTap(tap) {}

    functions::render::FrameTap& frameTap;
    std::vector<Slot>            slots;

    bool prepareSlot(Slot& slot, ID3D11Device* device, D3D11_TEXTURE2D_DESC const& sourceDesc) {
        bool const reusable = slot.device.Get() == device && slot.staging && slot.completionQuery
                           && slot.sourceDesc.Width == sourceDesc.Width
                           && slot.sourceDesc.Height == sourceDesc.Height && slot.sourceDesc.Format == sourceDesc.Format;
        if (reusable) return true;

        slot.device.Reset();
        slot.staging.Reset();
        slot.completionQuery.Reset();
        auto stagingDesc           = sourceDesc;
        stagingDesc.BindFlags      = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags      = 0;
        stagingDesc.Usage          = D3D11_USAGE_STAGING;
        D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0};
        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &slot.staging))
            || FAILED(device->CreateQuery(&queryDesc, &slot.completionQuery))) {
            slot.staging.Reset();
            slot.completionQuery.Reset();
            return false;
        }
        slot.device     = device;
        slot.sourceDesc = sourceDesc;
        return true;
    }
};

D3D11FrameTapBackend::D3D11FrameTapBackend(functions::render::FrameTap& frameTap)
: mImpl(std::make_unique<Impl>(frameTap)) {}

D3D11FrameTapBackend::~D3D11FrameTapBackend() = default;

void D3D11FrameTapBackend::poll(ID3D11DeviceContext* context) {
    if (!context) return;
    while (true) {
        Impl::Slot* slot{};
        for (auto& candidate : mImpl->slots) {
            if (!candidate.capture || !candidate.completionQuery || !candidate.staging) continue;
            if (!slot || candidate.capture->captureId < slot->capture->captureId) slot = &candidate;
        }
        if (!slot) return;

        HRESULT const ready = context->GetData(slot->completionQuery.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (ready == S_FALSE) return;
        if (FAILED(ready)) {
            HRESULT const deviceReason = slot->device ? slot->device->GetDeviceRemovedReason() : E_POINTER;
            auto const    error = FAILED(deviceReason) ? FrameTapError::DeviceLost : FrameTapError::MapFailed;
            getLogger().error(
                "D3D11 frame completion query failed (capture={}, frame={}, query=0x{:08X}, device=0x{:08X})",
                slot->capture->captureId,
                slot->capture->ticket.frameIndex,
                static_cast<uint32_t>(ready),
                static_cast<uint32_t>(deviceReason)
            );
            mImpl->frameTap.fail(*slot->capture, error, "D3D11 frame completion query failed");
            slot->capture.reset();
            slot->device.Reset();
            slot->staging.Reset();
            slot->completionQuery.Reset();
            continue;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT const            mappedResult =
            context->Map(slot->staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (mappedResult == DXGI_ERROR_WAS_STILL_DRAWING) return;
        if (FAILED(mappedResult)) {
            HRESULT const deviceReason = slot->device ? slot->device->GetDeviceRemovedReason() : E_POINTER;
            auto const    error = FAILED(deviceReason) ? FrameTapError::DeviceLost : FrameTapError::MapFailed;
            getLogger().error(
                "D3D11 readback Map failed (capture={}, frame={}, map=0x{:08X}, device=0x{:08X}, size={}x{})",
                slot->capture->captureId,
                slot->capture->ticket.frameIndex,
                static_cast<uint32_t>(mappedResult),
                static_cast<uint32_t>(deviceReason),
                slot->sourceDesc.Width,
                slot->sourceDesc.Height
            );
            mImpl->frameTap.fail(*slot->capture, error, "Unable to map a completed D3D11 frame");
            slot->capture.reset();
            slot->device.Reset();
            slot->staging.Reset();
            slot->completionQuery.Reset();
            continue;
        }

        CapturedFrame frame;
        frame.width          = slot->sourceDesc.Width;
        frame.height         = slot->sourceDesc.Height;
        frame.rowPitch       = frame.width * 4;
        frame.pixelFormat    = pixelFormat(slot->sourceDesc.Format);
        auto const byteCount = static_cast<uint64_t>(frame.rowPitch) * frame.height;
        if (byteCount > std::numeric_limits<size_t>::max()) {
            context->Unmap(slot->staging.Get(), 0);
            mImpl->frameTap.fail(*slot->capture, FrameTapError::MapFailed, "D3D11 captured frame is too large");
            slot->capture.reset();
            continue;
        }
        frame.pixels.resize(static_cast<size_t>(byteCount));
        for (uint32_t y = 0; y < frame.height; ++y) {
            auto const* source = static_cast<std::byte const*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
            auto*       target = frame.pixels.data() + static_cast<size_t>(y) * frame.rowPitch;
            std::memcpy(target, source, frame.rowPitch);
        }
        context->Unmap(slot->staging.Get(), 0);
        auto capture = *slot->capture;
        slot->capture.reset();
        mImpl->frameTap.complete(capture, std::move(frame));
    }
}

bool D3D11FrameTapBackend::capture(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source) {
    if (!device || !context || !source) return false;
    ComPtr<ID3D11Device> contextDevice;
    ComPtr<ID3D11Device> sourceDevice;
    context->GetDevice(contextDevice.GetAddressOf());
    source->GetDevice(sourceDevice.GetAddressOf());
    if (contextDevice.Get() != device || sourceDevice.Get() != device) {
        mImpl->frameTap.failActive(
            FrameTapError::BackendUnavailable,
            "The D3D11 frame source or context belongs to a different device"
        );
        return false;
    }
    poll(context);
    if (!mImpl->frameTap.requiresRenderPass()) return false;

    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    if (!isSupported(sourceDesc.Format) || sourceDesc.SampleDesc.Count != 1 || sourceDesc.Width == 0
        || sourceDesc.Height == 0) {
        mImpl->frameTap.failActive(FrameTapError::UnsupportedFormat, "Unsupported D3D11 frame format");
        return false;
    }

    auto const capacity = mImpl->frameTap.captureCapacity();
    if (mImpl->slots.size() < capacity) mImpl->slots.resize(capacity);
    auto slot =
        std::find_if(mImpl->slots.begin(), mImpl->slots.end(), [](Impl::Slot const& value) { return !value.capture; });
    if (slot == mImpl->slots.end()) return false;
    if (!mImpl->prepareSlot(*slot, device, sourceDesc)) {
        mImpl->frameTap.failActive(
            FrameTapError::BackendUnavailable,
            "Unable to allocate D3D11 frame readback resources"
        );
        return false;
    }

    auto capture = mImpl->frameTap.beginCapture();
    if (!capture) return false;
    slot->capture = *capture;
    context->CopyResource(slot->staging.Get(), source);
    context->End(slot->completionQuery.Get());
    return true;
}

void D3D11FrameTapBackend::reset(FrameTapError error, std::string message) {
    mImpl->frameTap.failActive(error, std::move(message));
    mImpl->slots.clear();
}

} // namespace playback::editor::renderer
