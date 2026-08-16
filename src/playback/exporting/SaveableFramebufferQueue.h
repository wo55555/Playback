#pragma once

#include "playback/visuals/FrameTap.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace playback::exporting {

enum class FrameDownloadQueueState : uint8_t { Closed, Open, Cancelled, Faulted };

enum class FrameDownloadRequestResult : uint8_t {
    Requested,
    Busy,
    Backpressured,
    Closed,
    InvalidTicket,
    Failed,
};

struct FrameDownloadQueueStatus {
    FrameDownloadQueueState state{FrameDownloadQueueState::Closed};
    visuals::FrameTapError  error{visuals::FrameTapError::None};
    std::string             message;
    visuals::FrameTapStatus tap;
    uint32_t                pendingDownloads{};
    uint32_t                inFlightDownloads{};
    uint32_t                readyDownloads{};
    bool                    renderRequested{};
};

class SaveableFramebufferQueue {
public:
    explicit SaveableFramebufferQueue(visuals::FrameTap& frameTap) : mFrameTap(frameTap) {}
    ~SaveableFramebufferQueue();

    SaveableFramebufferQueue(SaveableFramebufferQueue const&)            = delete;
    SaveableFramebufferQueue& operator=(SaveableFramebufferQueue const&) = delete;

    [[nodiscard]] bool open(uint32_t capacity = 4);
    void               close();
    void               cancel();

    [[nodiscard]] FrameDownloadRequestResult requestDownload(visuals::FrameTicket ticket);
    [[nodiscard]] bool                       hasDownloadStarted(visuals::FrameTicket const& ticket);

    [[nodiscard]] std::optional<visuals::CapturedFrame> finishDownload();

    [[nodiscard]] bool                     canRequestDownload();
    [[nodiscard]] bool                     isEmpty();
    [[nodiscard]] size_t                   pendingCount();
    [[nodiscard]] FrameDownloadQueueStatus status();

private:
    enum class DownloadState : uint8_t { Requested, Downloading };

    struct PendingDownload {
        visuals::FrameTicket                  ticket;
        DownloadState                         state{DownloadState::Requested};
        std::optional<visuals::CapturedFrame> downloaded;
        bool                                  startAcknowledged{};

        [[nodiscard]] bool acknowledgeStarted() {
            if (state != DownloadState::Downloading) return false;
            startAcknowledged = true;
            return true;
        }

        [[nodiscard]] bool canFinish() const { return startAcknowledged && downloaded.has_value(); }
    };

    void refresh();
    void fault(visuals::FrameTapError error, std::string message);

    visuals::FrameTap&                      mFrameTap;
    std::optional<visuals::FrameTapSession> mSession;
    std::deque<PendingDownload>             mPending;
    FrameDownloadQueueState                 mState{FrameDownloadQueueState::Closed};
    visuals::FrameTapError                  mError{visuals::FrameTapError::None};
    std::string                             mMessage;
    uint32_t                                mCapacity{};
};

} // namespace playback::exporting
