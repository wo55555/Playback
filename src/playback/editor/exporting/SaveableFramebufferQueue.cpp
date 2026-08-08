#include "SaveableFramebufferQueue.h"

#include "playback/Playback.h"

#include <algorithm>
#include <utility>

namespace playback::editor::exporting {

namespace {

bool ticketsEqual(functions::render::FrameTicket const& left, functions::render::FrameTicket const& right) {
    return left.frameIndex == right.frameIndex && left.ptsNumerator == right.ptsNumerator
        && left.ptsDenominator == right.ptsDenominator;
}

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

} // namespace

SaveableFramebufferQueue::~SaveableFramebufferQueue() { close(); }

bool SaveableFramebufferQueue::open(uint32_t capacity) {
    close();
    functions::render::FrameTapSession session;
    if (mFrameTap.open({capacity, false}, session) != functions::render::FrameTapOpenResult::Opened) return false;

    mSession = session;
    mState   = FrameDownloadQueueState::Open;
    mError   = functions::render::FrameTapError::None;
    mMessage.clear();
    mCapacity = capacity;
    return true;
}

void SaveableFramebufferQueue::close() {
    if (mSession) mFrameTap.close(*mSession);
    mSession.reset();
    mPending.clear();
    mState = FrameDownloadQueueState::Closed;
    mError = functions::render::FrameTapError::None;
    mMessage.clear();
    mCapacity = 0;
}

void SaveableFramebufferQueue::cancel() {
    if (mSession) {
        mFrameTap.cancel(*mSession);
        mFrameTap.close(*mSession);
    }
    mSession.reset();
    mPending.clear();
    mState    = FrameDownloadQueueState::Cancelled;
    mError    = functions::render::FrameTapError::Cancelled;
    mMessage  = "Framebuffer downloads were cancelled";
    mCapacity = 0;
}

FrameDownloadRequestResult SaveableFramebufferQueue::requestDownload(functions::render::FrameTicket ticket) {
    refresh();
    if (mState == FrameDownloadQueueState::Faulted) return FrameDownloadRequestResult::Failed;
    if (mState != FrameDownloadQueueState::Open || !mSession) return FrameDownloadRequestResult::Closed;
    if (ticket.ptsDenominator <= 0) return FrameDownloadRequestResult::InvalidTicket;
    if (mPending.size() >= mCapacity) return FrameDownloadRequestResult::Backpressured;
    if (std::ranges::any_of(mPending, [](PendingDownload const& pending) {
            return pending.state == DownloadState::Requested;
        })) {
        return FrameDownloadRequestResult::Busy;
    }

    switch (mFrameTap.tryArm(*mSession, ticket)) {
    case functions::render::FrameTapArmResult::Armed:
        mPending.push_back(PendingDownload{ticket});
        return FrameDownloadRequestResult::Requested;
    case functions::render::FrameTapArmResult::Busy:
        return FrameDownloadRequestResult::Busy;
    case functions::render::FrameTapArmResult::Backpressured:
        return FrameDownloadRequestResult::Backpressured;
    case functions::render::FrameTapArmResult::InvalidTicket:
        return FrameDownloadRequestResult::InvalidTicket;
    case functions::render::FrameTapArmResult::Inactive:
        fault(functions::render::FrameTapError::BackendUnavailable, "The frame capture session became inactive");
        return FrameDownloadRequestResult::Failed;
    }
    return FrameDownloadRequestResult::Failed;
}

bool SaveableFramebufferQueue::hasDownloadStarted(functions::render::FrameTicket const& ticket) {
    refresh();
    auto pending = std::ranges::find_if(mPending, [&ticket](PendingDownload const& candidate) {
        return ticketsEqual(candidate.ticket, ticket);
    });
    if (pending == mPending.end()) return false;

    if (!pending->acknowledgeStarted()) return false;
    return true;
}

std::optional<functions::render::CapturedFrame> SaveableFramebufferQueue::finishDownload() {
    refresh();
    if (mPending.empty() || !mPending.front().canFinish()) return std::nullopt;
    auto frame = std::move(*mPending.front().downloaded);
    mPending.pop_front();
    return frame;
}

bool SaveableFramebufferQueue::canRequestDownload() {
    refresh();
    if (mState != FrameDownloadQueueState::Open || !mSession || mPending.size() >= mCapacity) return false;
    return std::ranges::none_of(mPending, [](PendingDownload const& pending) {
        return pending.state == DownloadState::Requested;
    });
}

bool SaveableFramebufferQueue::isEmpty() {
    refresh();
    return mPending.empty();
}

size_t SaveableFramebufferQueue::pendingCount() {
    refresh();
    return mPending.size();
}

FrameDownloadQueueStatus SaveableFramebufferQueue::status() {
    refresh();
    FrameDownloadQueueStatus result;
    result.state   = mState;
    result.error   = mError;
    result.message = mMessage;
    if (mSession) result.tap = mFrameTap.status(*mSession);
    result.pendingDownloads = static_cast<uint32_t>(mPending.size());
    for (auto const& pending : mPending) {
        if (pending.downloaded) {
            ++result.readyDownloads;
        } else if (pending.state == DownloadState::Requested) {
            result.renderRequested = true;
        } else {
            ++result.inFlightDownloads;
        }
    }
    return result;
}

void SaveableFramebufferQueue::refresh() {
    if (mState != FrameDownloadQueueState::Open || !mSession) return;

    while (auto started = mFrameTap.tryPopStarted(*mSession)) {
        auto pending = std::ranges::find_if(mPending, [](PendingDownload const& candidate) {
            return candidate.state == DownloadState::Requested;
        });
        if (pending == mPending.end() || !ticketsEqual(pending->ticket, started->ticket)) {
            fault(functions::render::FrameTapError::BackendUnavailable, "The renderer started an unexpected frame");
            return;
        }
        pending->state = DownloadState::Downloading;
    }

    while (auto frame = mFrameTap.tryPop(*mSession)) {
        auto pending = std::ranges::find_if(mPending, [](PendingDownload const& candidate) {
            return candidate.state == DownloadState::Downloading && !candidate.downloaded;
        });
        if (pending == mPending.end() || !ticketsEqual(pending->ticket, frame->ticket)) {
            fault(functions::render::FrameTapError::BackendUnavailable, "The renderer completed frames out of order");
            return;
        }
        pending->downloaded = std::move(*frame);
    }

    auto const tapStatus = mFrameTap.status(*mSession);
    if (tapStatus.state == functions::render::FrameTapState::Faulted) {
        fault(tapStatus.error, tapStatus.message.empty() ? "The renderer frame download failed" : tapStatus.message);
    } else if (tapStatus.state == functions::render::FrameTapState::Cancelled) {
        mFrameTap.close(*mSession);
        mSession.reset();
        mPending.clear();
        mState    = FrameDownloadQueueState::Cancelled;
        mError    = functions::render::FrameTapError::Cancelled;
        mMessage  = tapStatus.message.empty() ? "Framebuffer downloads were cancelled" : tapStatus.message;
        mCapacity = 0;
    }
}

void SaveableFramebufferQueue::fault(functions::render::FrameTapError error, std::string message) {
    getLogger().error(
        "Framebuffer download queue failed (error {}, {} pending): {}",
        static_cast<int>(error),
        mPending.size(),
        message
    );
    if (mSession) {
        mFrameTap.cancel(*mSession);
        mFrameTap.close(*mSession);
    }
    mSession.reset();
    mPending.clear();
    mState    = FrameDownloadQueueState::Faulted;
    mError    = error;
    mMessage  = std::move(message);
    mCapacity = 0;
}

} // namespace playback::editor::exporting
