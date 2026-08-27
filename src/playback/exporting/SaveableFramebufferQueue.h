#pragma once

#include "playback/visuals/FrameCaptureTypes.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace playback::exporting {

class SaveableFramebuffer;

} // namespace playback::exporting

struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12Fence;
struct ID3D12Resource;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace playback::exporting {

enum class SaveableFramebufferQueueState : uint8_t { Closed, Open, Cancelled, Faulted };
enum class SaveableFramebufferBackend : uint8_t { None, D3D11, D3D12 };
enum class SaveableFramebufferQueueError : uint8_t {
    None,
    BackendUnavailable,
    UnsupportedFormat,
    Resize,
    DeviceLost,
    FenceFailed,
    MapFailed,
    Cancelled,
};
enum class SaveableFramebufferRequestResult : uint8_t { Requested, Busy, Backpressured, Closed, InvalidTicket, Failed };

struct SaveableFramebufferQueueStatus {
    SaveableFramebufferQueueState state{SaveableFramebufferQueueState::Closed};
    SaveableFramebufferQueueError error{SaveableFramebufferQueueError::None};
    std::string                   message;
    uint32_t                      pendingDownloads{};
    uint32_t                      inFlightDownloads{};
    uint32_t                      readyDownloads{};
    bool                          renderRequested{};
};

struct SaveableFramebufferPreview {
    SaveableFramebufferBackend backend{SaveableFramebufferBackend::None};
    ID3D11Texture2D*           d3d11Texture{};
    ID3D12Resource*            d3d12Resource{};
    ID3D12Fence*               d3d12Fence{};
    std::shared_ptr<void>      lifetime;
    uint64_t                   fenceValue{};
    uint64_t                   revision{};
    uint32_t                   width{};
    uint32_t                   height{};
    uint32_t                   format{};
};

class SaveableFramebufferQueue {
public:
    SaveableFramebufferQueue();
    ~SaveableFramebufferQueue();

    SaveableFramebufferQueue(SaveableFramebufferQueue const&)            = delete;
    SaveableFramebufferQueue& operator=(SaveableFramebufferQueue const&) = delete;

    [[nodiscard]] bool open(uint32_t capacity = 4);
    void               close();
    void               cancel();

    [[nodiscard]] SaveableFramebufferRequestResult requestDownload(visuals::FrameTicket ticket);
    [[nodiscard]] bool startDownload(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source);
    [[nodiscard]] bool
    startDownload(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* source, uint32_t sourceState);
    [[nodiscard]] bool                                  hasDownloadStarted(visuals::FrameTicket const& ticket) const;
    [[nodiscard]] std::optional<visuals::CapturedFrame> finishDownload();

    [[nodiscard]] bool                           canRequestDownload() const;
    [[nodiscard]] bool                           isEmpty() const;
    [[nodiscard]] size_t                         pendingCount() const;
    [[nodiscard]] SaveableFramebufferQueueStatus status() const;
    [[nodiscard]] SaveableFramebufferPreview     preview() const;

    void fail(SaveableFramebufferQueueError error, std::string message);

private:
    [[nodiscard]] bool ensurePreview(ID3D11Device* device, ID3D11Texture2D* source, std::string& error);
    [[nodiscard]] bool ensurePreview(ID3D12Device* device, ID3D12Resource* source, std::string& error);
    void closeLocked(SaveableFramebufferQueueState state, SaveableFramebufferQueueError error, std::string message);
    void faultLocked(SaveableFramebufferQueueError error, std::string message);

    mutable std::mutex                                mMutex;
    std::vector<std::unique_ptr<SaveableFramebuffer>> mAvailable;
    std::deque<std::unique_ptr<SaveableFramebuffer>>  mWaiting;
    struct PreviewResources;
    std::shared_ptr<PreviewResources>   mPreview;
    std::optional<visuals::FrameTicket> mRequestedTicket;
    uint64_t                            mPreviewRevision{};
    SaveableFramebufferQueueState       mState{SaveableFramebufferQueueState::Closed};
    SaveableFramebufferQueueError       mError{SaveableFramebufferQueueError::None};
    std::string                         mMessage;
    uint32_t                            mCapacity{};
};

} // namespace playback::exporting
