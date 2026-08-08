#include "OfflineRenderBoundary.h"

#include "playback/functions/replay/ReplaySession.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace playback::editor::exporting {

namespace {

bool ticketsEqual(functions::render::FrameTicket const& left, functions::render::FrameTicket const& right) {
    return left.frameIndex == right.frameIndex && left.ptsNumerator == right.ptsNumerator
        && left.ptsDenominator == right.ptsDenominator;
}

} // namespace

OfflineRenderBoundary::OfflineRenderBoundary(functions::ReplaySession& replay, functions::render::FrameTap& frameTap)
: mReplay(replay),
  mDownloads(frameTap) {}

OfflineRenderBoundary::~OfflineRenderBoundary() { close(); }

bool OfflineRenderBoundary::open(uint32_t capacity, int64_t maximumReplayTick) {
    close();
    if (!isOfflineRenderClockInstalled()) return false;
    if (!mDownloads.open(capacity)) return false;
    if (!functions::beginOfflineReplayTickGate()) {
        mDownloads.close();
        return false;
    }
    mTickGateOpen      = true;
    mMaximumReplayTick = std::max<int64_t>(0, maximumReplayTick);
    mState             = OfflineRenderBoundaryState::Ready;
    mError             = OfflineRenderBoundaryError::None;
    mMessage.clear();
    return true;
}

void OfflineRenderBoundary::close() {
    clearClockSample();
    mReplayTickToken.reset();
    if (mTickGateOpen) {
        functions::endOfflineReplayTickGate();
        mTickGateOpen = false;
    }
    mReplay.clearExportSeekRequest();
    mDownloads.close();
    mPendingFrame.reset();
    mLastSubmittedFrame.reset();
    mMaximumReplayTick = 0;
    mState             = OfflineRenderBoundaryState::Closed;
    mError             = OfflineRenderBoundaryError::None;
    mMessage.clear();
}

void OfflineRenderBoundary::cancel() {
    clearClockSample();
    mReplayTickToken.reset();
    if (mTickGateOpen) {
        functions::endOfflineReplayTickGate();
        mTickGateOpen = false;
    }
    mReplay.clearExportSeekRequest();
    mDownloads.cancel();
    mPendingFrame.reset();
    mLastSubmittedFrame.reset();
    mMaximumReplayTick = 0;
    mState             = OfflineRenderBoundaryState::Cancelled;
    mError             = OfflineRenderBoundaryError::None;
    mMessage           = "Offline rendering was cancelled";
}

OfflineRenderStepResult OfflineRenderBoundary::advance(ExportFramePlan const& frame) {
    if (mState == OfflineRenderBoundaryState::Faulted) return OfflineRenderStepResult::Failed;
    if (mState != OfflineRenderBoundaryState::Ready && mState != OfflineRenderBoundaryState::PreparingReplay
        && mState != OfflineRenderBoundaryState::AwaitingRender) {
        fault(OfflineRenderBoundaryError::InvalidState, "The offline renderer cannot accept another frame");
        return OfflineRenderStepResult::Failed;
    }
    if (frame.replayTickDenominator <= 0 || frame.ticket.ptsDenominator <= 0) {
        fault(OfflineRenderBoundaryError::InvalidFrame, "The offline render frame has an invalid time base");
        return OfflineRenderStepResult::Failed;
    }

    if (mPendingFrame && !ticketsEqual(mPendingFrame->ticket, frame.ticket)) {
        fault(OfflineRenderBoundaryError::InvalidFrame, "The offline render frame changed before it was submitted");
        return OfflineRenderStepResult::Failed;
    }
    if (!mPendingFrame) {
        if (!mDownloads.canRequestDownload()) return OfflineRenderStepResult::Backpressured;
        mPendingFrame = frame;
        mState        = OfflineRenderBoundaryState::PreparingReplay;
    }

    if (mState == OfflineRenderBoundaryState::PreparingReplay) {
        if (mReplayTickToken) {
            if (!functions::wasOfflineReplayTickCompleted(*mReplayTickToken)) {
                return OfflineRenderStepResult::Waiting;
            }
            mReplayTickToken.reset();
        }

        switch (mReplay.prepareExportTick(targetTick(*mPendingFrame))) {
        case functions::ReplayExportTickState::Unavailable:
            fault(OfflineRenderBoundaryError::ReplayUnavailable, "The replay became unavailable during export");
            return OfflineRenderStepResult::Failed;
        case functions::ReplayExportTickState::Failed:
            fault(OfflineRenderBoundaryError::ReplayFailed, "The replay failed while preparing an export frame");
            return OfflineRenderStepResult::Failed;
        case functions::ReplayExportTickState::Waiting: {
            functions::OfflineReplayTickToken token;
            switch (functions::requestOfflineReplayTick(token)) {
            case functions::OfflineReplayTickRequestResult::Requested:
                mReplayTickToken = token;
                return OfflineRenderStepResult::Waiting;
            case functions::OfflineReplayTickRequestResult::Unavailable:
                fault(OfflineRenderBoundaryError::TickUnavailable, "The offline replay tick gate is unavailable");
                return OfflineRenderStepResult::Failed;
            case functions::OfflineReplayTickRequestResult::Busy:
                fault(OfflineRenderBoundaryError::TickUnavailable, "The offline replay tick gate is already in use");
                return OfflineRenderStepResult::Failed;
            }
            return OfflineRenderStepResult::Waiting;
        }
        case functions::ReplayExportTickState::Ready:
            break;
        }

        if (!mClockToken) {
            auto const sample = clockSample(*mPendingFrame);
            if (!sample) {
                fault(OfflineRenderBoundaryError::InvalidFrame, "The offline render clock sample is invalid");
                return OfflineRenderStepResult::Failed;
            }

            OfflineRenderClockToken token;
            switch (publishOfflineRenderClockSample(*sample, token)) {
            case OfflineRenderClockPublishResult::Published:
                mClockToken = token;
                break;
            case OfflineRenderClockPublishResult::Unavailable:
                fault(OfflineRenderBoundaryError::ClockUnavailable, "The fractional render clock is unavailable");
                return OfflineRenderStepResult::Failed;
            case OfflineRenderClockPublishResult::Busy:
                fault(OfflineRenderBoundaryError::ClockUnavailable, "The fractional render clock is already in use");
                return OfflineRenderStepResult::Failed;
            case OfflineRenderClockPublishResult::InvalidSample:
                fault(
                    OfflineRenderBoundaryError::InvalidFrame,
                    "The fractional render clock rejected the frame sample"
                );
                return OfflineRenderStepResult::Failed;
            }
        }

        switch (mDownloads.requestDownload(mPendingFrame->ticket)) {
        case FrameDownloadRequestResult::Requested:
            mState = OfflineRenderBoundaryState::AwaitingRender;
            break;
        case FrameDownloadRequestResult::Busy:
            clearClockSample();
            return OfflineRenderStepResult::Waiting;
        case FrameDownloadRequestResult::Backpressured:
            clearClockSample();
            return OfflineRenderStepResult::Backpressured;
        case FrameDownloadRequestResult::Closed:
            fault(OfflineRenderBoundaryError::CaptureUnavailable, "The framebuffer download queue is closed");
            return OfflineRenderStepResult::Failed;
        case FrameDownloadRequestResult::InvalidTicket:
            fault(OfflineRenderBoundaryError::InvalidFrame, "The renderer rejected the export frame ticket");
            return OfflineRenderStepResult::Failed;
        case FrameDownloadRequestResult::Failed: {
            auto const downloadStatus = mDownloads.status();
            fault(
                OfflineRenderBoundaryError::CaptureFailed,
                downloadStatus.message.empty() ? "The framebuffer download request failed" : downloadStatus.message
            );
            return OfflineRenderStepResult::Failed;
        }
        }
    }

    if (!mDownloads.hasDownloadStarted(mPendingFrame->ticket)) {
        auto const downloadStatus = mDownloads.status();
        if (downloadStatus.state == FrameDownloadQueueState::Faulted) {
            fault(
                OfflineRenderBoundaryError::CaptureFailed,
                downloadStatus.message.empty() ? "The renderer frame download failed" : downloadStatus.message
            );
            return OfflineRenderStepResult::Failed;
        }
        if (downloadStatus.state == FrameDownloadQueueState::Cancelled
            || downloadStatus.state == FrameDownloadQueueState::Closed) {
            fault(
                OfflineRenderBoundaryError::CaptureUnavailable,
                downloadStatus.message.empty() ? "The framebuffer download queue became unavailable"
                                               : downloadStatus.message
            );
            return OfflineRenderStepResult::Failed;
        }
        return OfflineRenderStepResult::Waiting;
    }

    if (mClockToken && didOfflineRenderClockSampleFail(*mClockToken)) {
        fault(
            OfflineRenderBoundaryError::ClockUnavailable,
            "The renderer could not apply the fractional replay entity pose"
        );
        return OfflineRenderStepResult::Failed;
    }

    if (!mClockToken || !wasOfflineRenderClockSampleApplied(*mClockToken)) {
        fault(
            OfflineRenderBoundaryError::ClockUnavailable,
            "The captured frame completed before its fractional clock and entity pose were applied"
        );
        return OfflineRenderStepResult::Failed;
    }

    clearClockSample();
    mLastSubmittedFrame = mPendingFrame;
    mPendingFrame.reset();
    mState = OfflineRenderBoundaryState::Ready;
    return OfflineRenderStepResult::FrameSubmitted;
}

bool OfflineRenderBoundary::beginDrain() {
    if (mState != OfflineRenderBoundaryState::Ready || mPendingFrame) return false;
    clearClockSample();
    mReplay.clearExportSeekRequest();
    mState = OfflineRenderBoundaryState::Draining;
    return true;
}

bool OfflineRenderBoundary::isDrained() {
    if (mState != OfflineRenderBoundaryState::Draining) return false;
    return mDownloads.isEmpty();
}

std::optional<functions::render::CapturedFrame> OfflineRenderBoundary::finishDownload() {
    return mDownloads.finishDownload();
}

OfflineRenderBoundaryStatus OfflineRenderBoundary::status() {
    OfflineRenderBoundaryStatus result;
    result.state     = mState;
    result.error     = mError;
    result.message   = mMessage;
    result.downloads = mDownloads.status();
    if (result.state != OfflineRenderBoundaryState::Faulted) {
        if (result.downloads.state == FrameDownloadQueueState::Faulted) {
            fault(
                OfflineRenderBoundaryError::CaptureFailed,
                result.downloads.message.empty() ? "The renderer frame download failed" : result.downloads.message
            );
        } else if (result.state != OfflineRenderBoundaryState::Closed
                   && result.state != OfflineRenderBoundaryState::Cancelled
                   && (result.downloads.state == FrameDownloadQueueState::Closed
                       || result.downloads.state == FrameDownloadQueueState::Cancelled)) {
            fault(
                OfflineRenderBoundaryError::CaptureUnavailable,
                result.downloads.message.empty() ? "The framebuffer download queue became unavailable"
                                                 : result.downloads.message
            );
        }
    }
    if (mState == OfflineRenderBoundaryState::Faulted) {
        result.state   = mState;
        result.error   = mError;
        result.message = mMessage;
    }
    return result;
}

int OfflineRenderBoundary::targetTick(ExportFramePlan const& frame) const {
    auto const sample  = frame.replayTickNumerator / frame.replayTickDenominator;
    auto const clamped = std::clamp<int64_t>(sample, 0, mMaximumReplayTick);
    return clamped > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(clamped);
}

std::optional<OfflineRenderClockSample> OfflineRenderBoundary::clockSample(ExportFramePlan const& frame) const {
    if (frame.replayTickNumerator < 0 || frame.replayTickDenominator <= 0) return std::nullopt;

    long double const current =
        static_cast<long double>(frame.replayTickNumerator) / static_cast<long double>(frame.replayTickDenominator);
    if (current < 0.0L || current > static_cast<long double>(mMaximumReplayTick)) return std::nullopt;

    auto const remainder = frame.replayTickNumerator % frame.replayTickDenominator;
    float      partial =
        static_cast<float>(static_cast<long double>(remainder) / static_cast<long double>(frame.replayTickDenominator));
    if (remainder != 0 && partial >= 1.0f) partial = std::nextafter(1.0f, 0.0f);

    long double delta = 0.0L;
    if (mLastSubmittedFrame) {
        delta = current
              - static_cast<long double>(mLastSubmittedFrame->replayTickNumerator)
                    / static_cast<long double>(mLastSubmittedFrame->replayTickDenominator);
    }
    if (delta < 0.0L || delta > static_cast<long double>(std::numeric_limits<float>::max())) return std::nullopt;

    return OfflineRenderClockSample{frame.ticket.frameIndex, partial, static_cast<float>(delta)};
}

void OfflineRenderBoundary::clearClockSample() {
    if (!mClockToken) return;
    clearOfflineRenderClockSample(*mClockToken);
    mClockToken.reset();
}

void OfflineRenderBoundary::fault(OfflineRenderBoundaryError error, std::string message) {
    clearClockSample();
    mReplayTickToken.reset();
    if (mTickGateOpen) {
        functions::endOfflineReplayTickGate();
        mTickGateOpen = false;
    }
    mReplay.clearExportSeekRequest();
    mDownloads.cancel();
    mPendingFrame.reset();
    mState   = OfflineRenderBoundaryState::Faulted;
    mError   = error;
    mMessage = std::move(message);
}

} // namespace playback::editor::exporting
