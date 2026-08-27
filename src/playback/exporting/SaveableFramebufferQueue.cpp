#include "SaveableFramebufferQueue.h"

#include "SaveableFramebuffer.h"

#include "playback/Playback.h"

#include <algorithm>
#include <utility>

namespace playback::exporting {

struct SaveableFramebufferQueue::PreviewResources {
    SaveableFramebufferBackend              backend{SaveableFramebufferBackend::None};
    Microsoft::WRL::ComPtr<ID3D11Device>    d3d11Device;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11Texture;
    Microsoft::WRL::ComPtr<ID3D12Device>    device;
    Microsoft::WRL::ComPtr<ID3D12Resource>  texture;
    Microsoft::WRL::ComPtr<ID3D12Fence>     fence;
    uint64_t                                fenceValue{};
};

namespace {

bool ticketsEqual(visuals::FrameTicket const& left, visuals::FrameTicket const& right) {
    return left.frameIndex == right.frameIndex && left.ptsNumerator == right.ptsNumerator
        && left.ptsDenominator == right.ptsDenominator;
}


auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

} // namespace

SaveableFramebufferQueue::SaveableFramebufferQueue() = default;

SaveableFramebufferQueue::~SaveableFramebufferQueue() { close(); }

bool SaveableFramebufferQueue::open(uint32_t capacity) {
    close();
    if (capacity == 0 || capacity > 8) return false;

    std::scoped_lock lock(mMutex);
    mAvailable.reserve(capacity);
    for (uint32_t index = 0; index < capacity; ++index) {
        mAvailable.emplace_back(std::make_unique<SaveableFramebuffer>());
    }
    mCapacity = capacity;
    mState    = SaveableFramebufferQueueState::Open;
    mError    = SaveableFramebufferQueueError::None;
    mMessage.clear();
    return true;
}

void SaveableFramebufferQueue::close() {
    std::scoped_lock lock(mMutex);
    closeLocked(SaveableFramebufferQueueState::Closed, SaveableFramebufferQueueError::None, {});
}

void SaveableFramebufferQueue::cancel() {
    std::scoped_lock lock(mMutex);
    closeLocked(
        SaveableFramebufferQueueState::Cancelled,
        SaveableFramebufferQueueError::Cancelled,
        "Framebuffer downloads were cancelled"
    );
}

SaveableFramebufferRequestResult SaveableFramebufferQueue::requestDownload(visuals::FrameTicket ticket) {
    std::scoped_lock lock(mMutex);
    if (mState == SaveableFramebufferQueueState::Faulted) return SaveableFramebufferRequestResult::Failed;
    if (mState != SaveableFramebufferQueueState::Open) return SaveableFramebufferRequestResult::Closed;
    if (ticket.ptsDenominator <= 0) return SaveableFramebufferRequestResult::InvalidTicket;
    if (mRequestedTicket) return SaveableFramebufferRequestResult::Busy;
    if (mAvailable.empty() || mWaiting.size() >= mCapacity) return SaveableFramebufferRequestResult::Backpressured;
    mRequestedTicket = ticket;
    return SaveableFramebufferRequestResult::Requested;
}

bool SaveableFramebufferQueue::ensurePreview(ID3D11Device* device, ID3D11Texture2D* source, std::string& error) {
    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    if (mPreview && mPreview->backend == SaveableFramebufferBackend::D3D11 && mPreview->d3d11Texture
        && mPreview->d3d11Device.Get() == device) {
        D3D11_TEXTURE2D_DESC current{};
        mPreview->d3d11Texture->GetDesc(&current);
        if (current.Width == sourceDesc.Width && current.Height == sourceDesc.Height
            && current.Format == sourceDesc.Format) {
            return true;
        }
    }
    if (!mWaiting.empty()) {
        error = "The framebuffer preview backend or dimensions cannot change while downloads are in flight";
        return false;
    }

    D3D11_TEXTURE2D_DESC previewDesc = sourceDesc;
    previewDesc.SampleDesc.Count     = 1;
    previewDesc.SampleDesc.Quality   = 0;
    previewDesc.Usage                = D3D11_USAGE_DEFAULT;
    previewDesc.BindFlags            = D3D11_BIND_SHADER_RESOURCE;
    previewDesc.CPUAccessFlags       = 0;
    previewDesc.MiscFlags            = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> preview;
    if (FAILED(device->CreateTexture2D(&previewDesc, nullptr, &preview))) {
        error = "Unable to allocate the D3D11 export preview texture";
        return false;
    }

    auto resources          = std::make_shared<PreviewResources>();
    resources->backend      = SaveableFramebufferBackend::D3D11;
    resources->d3d11Device  = device;
    resources->d3d11Texture = std::move(preview);
    mPreview                = std::move(resources);
    ++mPreviewRevision;
    return true;
}

bool SaveableFramebufferQueue::ensurePreview(ID3D12Device* device, ID3D12Resource* source, std::string& error) {
    auto const sourceDesc = source->GetDesc();
    auto const current    = mPreview && mPreview->texture ? mPreview->texture->GetDesc() : D3D12_RESOURCE_DESC{};
    if (mPreview && mPreview->backend == SaveableFramebufferBackend::D3D12 && mPreview->texture
        && mPreview->device.Get() == device && current.Width == sourceDesc.Width && current.Height == sourceDesc.Height
        && current.Format == sourceDesc.Format) {
        return true;
    }
    if (!mWaiting.empty()) {
        error = "The framebuffer preview backend or dimensions cannot change while downloads are in flight";
        return false;
    }

    D3D12_RESOURCE_DESC previewDesc = sourceDesc;
    previewDesc.Alignment           = 0;
    previewDesc.SampleDesc.Count    = 1;
    previewDesc.SampleDesc.Quality  = 0;
    previewDesc.Flags               = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES const
        heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    Microsoft::WRL::ComPtr<ID3D12Resource> preview;
    if (FAILED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &previewDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            nullptr,
            IID_PPV_ARGS(&preview)
        ))) {
        error = "Unable to allocate the D3D12 export preview texture";
        return false;
    }
    auto resources     = std::make_shared<PreviewResources>();
    resources->backend = SaveableFramebufferBackend::D3D12;
    resources->device  = device;
    resources->texture = std::move(preview);
    mPreview           = std::move(resources);
    ++mPreviewRevision;
    return true;
}

bool SaveableFramebufferQueue::startDownload(
    ID3D11Device*        device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D*     source
) {
    std::scoped_lock lock(mMutex);
    if (mState != SaveableFramebufferQueueState::Open || !mRequestedTicket) return false;
    if (!device || !context || !source || mAvailable.empty()) {
        faultLocked(
            SaveableFramebufferQueueError::BackendUnavailable,
            "The D3D11 framebuffer download backend is unavailable"
        );
        return false;
    }

    std::string error;
    if (!ensurePreview(device, source, error)) {
        faultLocked(SaveableFramebufferQueueError::BackendUnavailable, std::move(error));
        return false;
    }

    auto       slot   = std::move(mAvailable.back());
    auto const ticket = *mRequestedTicket;
    mAvailable.pop_back();
    if (!slot->submit(device, context, source, mPreview->d3d11Texture.Get(), ticket, error)) {
        mAvailable.emplace_back(std::move(slot));
        faultLocked(SaveableFramebufferQueueError::BackendUnavailable, std::move(error));
        return false;
    }

    mRequestedTicket.reset();
    mWaiting.emplace_back(std::move(slot));
    return true;
}

bool SaveableFramebufferQueue::startDownload(
    ID3D12Device*       device,
    ID3D12CommandQueue* queue,
    ID3D12Resource*     source,
    uint32_t            sourceState
) {
    std::scoped_lock lock(mMutex);
    if (mState != SaveableFramebufferQueueState::Open || !mRequestedTicket) return false;
    if (!device || !queue || !source || mAvailable.empty()) {
        faultLocked(
            SaveableFramebufferQueueError::BackendUnavailable,
            "The D3D12 framebuffer download backend is unavailable"
        );
        return false;
    }

    std::string error;
    if (!ensurePreview(device, source, error)) {
        faultLocked(SaveableFramebufferQueueError::BackendUnavailable, std::move(error));
        return false;
    }

    auto slot = std::move(mAvailable.back());
    mAvailable.pop_back();
    auto const ticket = *mRequestedTicket;
    if (!slot->submit(
            device,
            queue,
            source,
            static_cast<D3D12_RESOURCE_STATES>(sourceState),
            mPreview->texture.Get(),
            ticket,
            error
        )) {
        mAvailable.emplace_back(std::move(slot));
        faultLocked(SaveableFramebufferQueueError::BackendUnavailable, std::move(error));
        return false;
    }

    mRequestedTicket.reset();
    mPreview->fence      = slot->fence();
    mPreview->fenceValue = slot->fenceValue();
    mWaiting.emplace_back(std::move(slot));
    return true;
}

bool SaveableFramebufferQueue::hasDownloadStarted(visuals::FrameTicket const& ticket) const {
    std::scoped_lock lock(mMutex);
    return std::ranges::any_of(mWaiting, [&ticket](auto const& slot) { return ticketsEqual(slot->ticket(), ticket); });
}

std::optional<visuals::CapturedFrame> SaveableFramebufferQueue::finishDownload() {
    std::scoped_lock lock(mMutex);
    if (mState != SaveableFramebufferQueueState::Open || mWaiting.empty() || !mWaiting.front()->isComplete()) {
        return std::nullopt;
    }

    auto slot = std::move(mWaiting.front());
    mWaiting.pop_front();
    std::string error;
    auto        frame = slot->finish(error);
    if (!frame) {
        mAvailable.emplace_back(std::move(slot));
        faultLocked(SaveableFramebufferQueueError::MapFailed, std::move(error));
        return std::nullopt;
    }
    mAvailable.emplace_back(std::move(slot));
    return frame;
}

bool SaveableFramebufferQueue::canRequestDownload() const {
    std::scoped_lock lock(mMutex);
    return mState == SaveableFramebufferQueueState::Open && !mRequestedTicket && !mAvailable.empty()
        && mWaiting.size() < mCapacity;
}

bool SaveableFramebufferQueue::isEmpty() const {
    std::scoped_lock lock(mMutex);
    return !mRequestedTicket && mWaiting.empty();
}

size_t SaveableFramebufferQueue::pendingCount() const {
    std::scoped_lock lock(mMutex);
    return mWaiting.size() + static_cast<size_t>(mRequestedTicket.has_value());
}

SaveableFramebufferQueueStatus SaveableFramebufferQueue::status() const {
    std::scoped_lock               lock(mMutex);
    SaveableFramebufferQueueStatus result;
    result.state   = mState;
    result.error   = mError;
    result.message = mMessage;
    result.pendingDownloads =
        static_cast<uint32_t>(mWaiting.size()) + static_cast<uint32_t>(mRequestedTicket.has_value());
    result.inFlightDownloads = static_cast<uint32_t>(mWaiting.size());
    result.renderRequested   = mRequestedTicket.has_value();
    for (auto const& slot : mWaiting) {
        if (slot->isComplete()) ++result.readyDownloads;
    }
    result.inFlightDownloads -= result.readyDownloads;
    return result;
}

SaveableFramebufferPreview SaveableFramebufferQueue::preview() const {
    std::scoped_lock           lock(mMutex);
    SaveableFramebufferPreview result;
    result.backend       = mPreview ? mPreview->backend : SaveableFramebufferBackend::None;
    result.d3d11Texture  = mPreview ? mPreview->d3d11Texture.Get() : nullptr;
    result.d3d12Resource = mPreview ? mPreview->texture.Get() : nullptr;
    result.d3d12Fence    = mPreview ? mPreview->fence.Get() : nullptr;
    result.lifetime      = mPreview;
    result.fenceValue    = mPreview ? mPreview->fenceValue : 0;
    result.revision      = mPreviewRevision;
    if (mPreview && mPreview->backend == SaveableFramebufferBackend::D3D11 && mPreview->d3d11Texture) {
        D3D11_TEXTURE2D_DESC desc{};
        mPreview->d3d11Texture->GetDesc(&desc);
        result.width  = desc.Width;
        result.height = desc.Height;
        result.format = static_cast<uint32_t>(desc.Format);
    } else if (mPreview && mPreview->texture) {
        auto const desc = mPreview->texture->GetDesc();
        result.width    = static_cast<uint32_t>(desc.Width);
        result.height   = desc.Height;
        result.format   = static_cast<uint32_t>(desc.Format);
    }
    return result;
}

void SaveableFramebufferQueue::fail(SaveableFramebufferQueueError error, std::string message) {
    std::scoped_lock lock(mMutex);
    if (mState != SaveableFramebufferQueueState::Open) return;
    faultLocked(error, std::move(message));
}

void SaveableFramebufferQueue::closeLocked(
    SaveableFramebufferQueueState state,
    SaveableFramebufferQueueError error,
    std::string                   message
) {
    for (auto& slot : mWaiting) (void)slot->waitForCompletion();
    mWaiting.clear();
    mAvailable.clear();
    mRequestedTicket.reset();
    mPreview.reset();
    ++mPreviewRevision;
    mCapacity = 0;
    mState    = state;
    mError    = error;
    mMessage  = std::move(message);
}

void SaveableFramebufferQueue::faultLocked(SaveableFramebufferQueueError error, std::string message) {
    getLogger().error(
        "Framebuffer download queue failed (error {}, {} pending): {}",
        static_cast<int>(error),
        mWaiting.size() + static_cast<size_t>(mRequestedTicket.has_value()),
        message
    );
    closeLocked(SaveableFramebufferQueueState::Faulted, error, std::move(message));
}

} // namespace playback::exporting
