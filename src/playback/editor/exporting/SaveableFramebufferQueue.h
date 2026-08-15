#pragma once

#include "playback/functions/render/FrameTap.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace playback::editor::exporting {

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
    FrameDownloadQueueState           state{FrameDownloadQueueState::Closed};
    functions::render::FrameTapError  error{functions::render::FrameTapError::None};
    std::string                       message;
    functions::render::FrameTapStatus tap;
    uint32_t                          pendingDownloads{};
    uint32_t                          inFlightDownloads{};
    uint32_t                          readyDownloads{};
    bool                              renderRequested{};
};

class SaveableFramebufferQueue {
public:
    explicit SaveableFramebufferQueue(functions::render::FrameTap& frameTap) : mFrameTap(frameTap) {}
    ~SaveableFramebufferQueue();

    SaveableFramebufferQueue(SaveableFramebufferQueue const&)            = delete;
    SaveableFramebufferQueue& operator=(SaveableFramebufferQueue const&) = delete;

    [[nodiscard]] bool open(uint32_t capacity = 4);
    void               close();
    void               cancel();

    [[nodiscard]] FrameDownloadRequestResult requestDownload(functions::render::FrameTicket ticket);
    [[nodiscard]] bool                       hasDownloadStarted(functions::render::FrameTicket const& ticket);

    [[nodiscard]] std::optional<functions::render::CapturedFrame> finishDownload();

    [[nodiscard]] bool                     canRequestDownload();
    [[nodiscard]] bool                     isEmpty();
    [[nodiscard]] size_t                   pendingCount();
    [[nodiscard]] FrameDownloadQueueStatus status();

private:
    enum class DownloadState : uint8_t { Requested, Downloading };

    struct PendingDownload {
        functions::render::FrameTicket                  ticket;
        DownloadState                                   state{DownloadState::Requested};
        std::optional<functions::render::CapturedFrame> downloaded;
        bool                                            startAcknowledged{};

        [[nodiscard]] bool acknowledgeStarted() {
            if (state != DownloadState::Downloading) return false;
            startAcknowledged = true;
            return true;
        }

        [[nodiscard]] bool canFinish() const { return startAcknowledged && downloaded.has_value(); }
    };

    void refresh();
    void fault(functions::render::FrameTapError error, std::string message);

    functions::render::FrameTap&                      mFrameTap;
    std::optional<functions::render::FrameTapSession> mSession;
    std::deque<PendingDownload>                       mPending;
    FrameDownloadQueueState                           mState{FrameDownloadQueueState::Closed};
    functions::render::FrameTapError                  mError{functions::render::FrameTapError::None};
    std::string                                       mMessage;
    uint32_t                                          mCapacity{};
};

} // namespace playback::editor::exporting
