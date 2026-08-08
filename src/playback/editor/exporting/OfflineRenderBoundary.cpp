#include "OfflineRenderBoundary.h"

#include "playback/Playback.h"
#include "ExportActivity.h"
#include "playback/functions/render/ReplaySampleTime.h"
#include "playback/functions/replay/ReplaySession.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace playback::editor::exporting {

namespace {

constexpr uint32_t MaxRenderWaitFrames   = 600;
constexpr uint32_t RenderWaitLogInterval = 30;
constexpr uint32_t MaxCaptureRetries     = 2;

bool ticketsEqual(functions::render::FrameTicket const& left, functions::render::FrameTicket const& right) {
    return left.frameIndex == right.frameIndex && left.ptsNumerator == right.ptsNumerator
        && left.ptsDenominator == right.ptsDenominator;
}

} // namespace

OfflineRenderBoundary::OfflineRenderBoundary(functions::ReplaySession& replay, functions::render::FrameTap& frameTap)
: mReplay(replay),
  mDownloads(frameTap) {}

OfflineRenderBoundary::~OfflineRenderBoundary() { close(); }

bool OfflineRenderBoundary::open(
    uint32_t                              capacity,
    ExportSettings const&                 settings,
    editing::model::EditorStateExt const& project
) {
    close();
    setOfflineRenderActivityActive(false);
    if (!isOfflineRenderClockInstalled()) return false;
    if (!mExecutor.open(settings, project)) return false;
    if (!mDownloads.open(capacity)) {
        mExecutor.close();
        return false;
    }
    mCaptureCapacity = capacity;

    mMaximumReplayTick        = std::max<int64_t>(0, settings.endTick);
    auto const maximumIntTick = std::min<int64_t>(mMaximumReplayTick, std::numeric_limits<int>::max());
    auto const startTick      = std::clamp<int64_t>(settings.startTick, 0, maximumIntTick);
    if (!mReplay.beginExportTimeline(static_cast<int>(startTick))) {
        mReplay.endExportTimeline();
        mDownloads.close();
        mExecutor.close();
        mMaximumReplayTick = 0;
        mCaptureCapacity   = 0;
        return false;
    }

    mTimelineInitialized        = false;
    mInitializationTickObserved = false;
    mWarmupFramesRemaining      = settings.warmupFrames;
    mState                      = OfflineRenderBoundaryState::Ready;
    mError                      = OfflineRenderBoundaryError::None;
    mMessage.clear();
    return true;
}

void OfflineRenderBoundary::close() {
    setOfflineRenderActivityActive(false);
    clearClockSample();
    mReplayTickToken.reset();
    if (mTickGateOpen) {
        functions::endOfflineReplayTickGate();
        mTickGateOpen = false;
    }
    mReplay.endExportTimeline();
    mDownloads.close();
    mExecutor.close();
    mPendingFrame.reset();
    mLastSubmittedFrame.reset();
    mCompletedFrameTicket.reset();
    mMaximumReplayTick          = 0;
    mCaptureCapacity            = 0;
    mCaptureRetryCount          = 0;
    mWarmupFramesRemaining      = 0;
    mRenderWaitFrames           = 0;
    mTimelineInitialized        = false;
    mInitializationTickObserved = false;
    mState                      = OfflineRenderBoundaryState::Closed;
    mError                      = OfflineRenderBoundaryError::None;
    mMessage.clear();
}

void OfflineRenderBoundary::cancel() {
    setOfflineRenderActivityActive(false);
    clearClockSample();
    mReplayTickToken.reset();
    if (mTickGateOpen) {
        functions::endOfflineReplayTickGate();
        mTickGateOpen = false;
    }
    mReplay.endExportTimeline();
    mDownloads.cancel();
    mExecutor.close();
    mPendingFrame.reset();
    mLastSubmittedFrame.reset();
    mCompletedFrameTicket.reset();
    mMaximumReplayTick          = 0;
    mCaptureCapacity            = 0;
    mCaptureRetryCount          = 0;
    mWarmupFramesRemaining      = 0;
    mRenderWaitFrames           = 0;
    mTimelineInitialized        = false;
    mInitializationTickObserved = false;
    mState                      = OfflineRenderBoundaryState::Cancelled;
    mError                      = OfflineRenderBoundaryError::None;
    mMessage                    = "Offline rendering was cancelled";
}

OfflineRenderStepResult OfflineRenderBoundary::advance(ExportFramePlan const& frame) {
    if (mState == OfflineRenderBoundaryState::Faulted) return OfflineRenderStepResult::Failed;
    if (mState != OfflineRenderBoundaryState::Ready && mState != OfflineRenderBoundaryState::InitializingReplay
        && mState != OfflineRenderBoundaryState::PreparingReplay && mState != OfflineRenderBoundaryState::WarmingUp
        && mState != OfflineRenderBoundaryState::AwaitingDownload) {
        fault(OfflineRenderBoundaryError::InvalidState, "The offline renderer cannot accept another frame");
        return OfflineRenderStepResult::Failed;
    }
    if (frame.replayTickDenominator <= 0 || frame.ticket.ptsDenominator <= 0) {
        fault(OfflineRenderBoundaryError::InvalidFrame, "The offline render frame has an invalid time base");
        return OfflineRenderStepResult::Failed;
    }
    if (mCompletedFrameTicket) {
        if (!ticketsEqual(*mCompletedFrameTicket, frame.ticket)) {
            fault(OfflineRenderBoundaryError::InvalidFrame, "The completed render sample was not acknowledged in order");
            return OfflineRenderStepResult::Failed;
        }
        mCompletedFrameTicket.reset();
        return OfflineRenderStepResult::FrameSubmitted;
    }

    if (mPendingFrame && !ticketsEqual(mPendingFrame->ticket, frame.ticket)) {
        fault(OfflineRenderBoundaryError::InvalidFrame, "The offline render frame changed before it was submitted");
        return OfflineRenderStepResult::Failed;
    }
    if (!mPendingFrame) {
        if (!mDownloads.canRequestDownload()) return OfflineRenderStepResult::Backpressured;
        mPendingFrame = frame;
        mState        = !mTimelineInitialized       ? OfflineRenderBoundaryState::InitializingReplay
                      : mWarmupFramesRemaining != 0 ? OfflineRenderBoundaryState::WarmingUp
                                                    : OfflineRenderBoundaryState::PreparingReplay;
    }

    // Let the ordinary client/update and level-tick path finish the initial seek
    // (including a native dimension handshake) before the offline gate suppresses
    // unrequested ticks. Once the integer replay state is stable, every later
    // sample is driven exclusively by the explicit tick/render boundary below.
    if (!mTickGateOpen
        && (mState == OfflineRenderBoundaryState::InitializingReplay
            || mState == OfflineRenderBoundaryState::PreparingReplay)) {
        switch (mReplay.prepareExportTick(targetTick(*mPendingFrame))) {
        case functions::ReplayExportTickState::Unavailable:
            fault(OfflineRenderBoundaryError::ReplayUnavailable, "The replay became unavailable during export");
            return OfflineRenderStepResult::Failed;
        case functions::ReplayExportTickState::Invalid:
            fault(
                OfflineRenderBoundaryError::InvalidFrame,
                "The export frame moved backwards or changed its initialization tick"
            );
            return OfflineRenderStepResult::Failed;
        case functions::ReplayExportTickState::Failed:
            fault(OfflineRenderBoundaryError::ReplayFailed, "The replay failed while preparing an export frame");
            return OfflineRenderStepResult::Failed;
        case functions::ReplayExportTickState::Waiting:
            return OfflineRenderStepResult::Waiting;
        case functions::ReplayExportTickState::Ready:
            break;
        }

        if (!mTimelineInitialized) {
            if (!mReplay.finishExportTimelineInitialization()) {
                fault(
                    OfflineRenderBoundaryError::ReplayFailed,
                    "The replay was not stable after export initialization"
                );
                return OfflineRenderStepResult::Failed;
            }
            mTimelineInitialized        = true;
            mInitializationTickObserved = false;
        }

        if (!functions::beginOfflineReplayTickGate()) {
            fault(OfflineRenderBoundaryError::TickUnavailable, "The offline replay tick gate is unavailable");
            return OfflineRenderStepResult::Failed;
        }
        mTickGateOpen = true;
        setOfflineRenderActivityActive(true);
        mState = OfflineRenderBoundaryState::PreparingReplay;
    }

    if (mState == OfflineRenderBoundaryState::InitializingReplay
        || mState == OfflineRenderBoundaryState::PreparingReplay) {
        if (mReplayTickToken) {
            if (!functions::wasOfflineReplayTickCompleted(*mReplayTickToken)) {
                return OfflineRenderStepResult::Waiting;
            }

            auto const completion = functions::getOfflineReplayTickCompletion(*mReplayTickToken);
            if (!completion || !completion->clientTickExecuted) {
                fault(
                    OfflineRenderBoundaryError::TickUnavailable,
                    "The offline replay tick completed without a native client tick"
                );
                return OfflineRenderStepResult::Failed;
            }
            int const advancedTicks = completion->replayTicksAdvanced();
            if (mTimelineInitialized && (advancedTicks < 0 || advancedTicks > 1)) {
                fault(
                    OfflineRenderBoundaryError::TickUnavailable,
                    "A continuous export client tick advanced more than one replay tick"
                );
                return OfflineRenderStepResult::Failed;
            }
            if (!mTimelineInitialized) mInitializationTickObserved = true;
            mReplayTickToken.reset();
        }

        auto requestReplayTick = [&]() -> OfflineRenderStepResult {
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
        };

        switch (mReplay.prepareExportTick(targetTick(*mPendingFrame))) {
        case functions::ReplayExportTickState::Unavailable:
            fault(OfflineRenderBoundaryError::ReplayUnavailable, "The replay became unavailable during export");
            return OfflineRenderStepResult::Failed;
        case functions::ReplayExportTickState::Invalid:
            fault(
                OfflineRenderBoundaryError::InvalidFrame,
                "The export frame moved backwards or changed its initialization tick"
            );
            return OfflineRenderStepResult::Failed;
        case functions::ReplayExportTickState::Failed:
            fault(OfflineRenderBoundaryError::ReplayFailed, "The replay failed while preparing an export frame");
            return OfflineRenderStepResult::Failed;
        case functions::ReplayExportTickState::Waiting:
            return requestReplayTick();
        case functions::ReplayExportTickState::Ready:
            break;
        }

        if (!mTimelineInitialized) {
            if (!mInitializationTickObserved) return requestReplayTick();
            if (!mReplay.finishExportTimelineInitialization()) {
                fault(
                    OfflineRenderBoundaryError::ReplayFailed,
                    "The replay was not stable after export initialization"
                );
                return OfflineRenderStepResult::Failed;
            }
            mTimelineInitialized        = true;
            mInitializationTickObserved = false;
            mState                      = OfflineRenderBoundaryState::PreparingReplay;
        }

        if (mWarmupFramesRemaining != 0) {
            mState = OfflineRenderBoundaryState::WarmingUp;
            return advanceWarmup(*mPendingFrame);
        }

        if (!mClockToken) {
            if (!publishClockSample(*mPendingFrame)) return OfflineRenderStepResult::Failed;
        }

        switch (mDownloads.requestDownload(mPendingFrame->ticket)) {
        case FrameDownloadRequestResult::Requested:
            mState            = OfflineRenderBoundaryState::AwaitingDownload;
            mRenderWaitFrames = 0;
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

    if (mState == OfflineRenderBoundaryState::WarmingUp) return advanceWarmup(*mPendingFrame);

    if (mState == OfflineRenderBoundaryState::AwaitingDownload) {
        if (!mClockToken) {
            fault(OfflineRenderBoundaryError::ClockUnavailable, "The explicit offline render lost its clock sample");
            return OfflineRenderStepResult::Failed;
        }
        switch (mExecutor.executeSample(*mPendingFrame, *mClockToken)) {
        case OfflineRenderFrameExecutionResult::Waiting:
            return OfflineRenderStepResult::Waiting;
        case OfflineRenderFrameExecutionResult::Executed:
            break;
        case OfflineRenderFrameExecutionResult::Failed: {
            auto const executorStatus = mExecutor.status();
            fault(
                OfflineRenderBoundaryError::CaptureUnavailable,
                executorStatus.message.empty() ? "The explicit offline render failed" : executorStatus.message
            );
            return OfflineRenderStepResult::Failed;
        }
        }
    }

    bool const downloadStarted = mDownloads.hasDownloadStarted(mPendingFrame->ticket);
    if (!downloadStarted) {
        auto const downloadStatus = mDownloads.status();
        if (downloadStatus.state == FrameDownloadQueueState::Faulted) {
            if (recoverDownloadFailure(downloadStatus)) return OfflineRenderStepResult::Waiting;
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
        if (mState == OfflineRenderBoundaryState::AwaitingDownload && downloadStatus.renderRequested) {
            ++mRenderWaitFrames;
            if (mRenderWaitFrames == 1 || mRenderWaitFrames % RenderWaitLogInterval == 0) {
                Playback::getInstance().getSelf().getLogger().debug(
                    "Offline render waiting for BGFX flip/FrameTap (waitFrames={}, frame={}, tapArmed={}, "
                    "tapInFlight={}, renderSize={}x{})",
                    mRenderWaitFrames,
                    mPendingFrame->ticket.frameIndex,
                    downloadStatus.tap.armed,
                    downloadStatus.tap.inFlightFrames,
                    mExecutor.status().renderWidth,
                    mExecutor.status().renderHeight
                );
            }
            if (mRenderWaitFrames > MaxRenderWaitFrames) {
                fault(
                    OfflineRenderBoundaryError::CaptureFailed,
                    "The matching FrameTap capture did not start after waiting for 600 render frames"
                );
                return OfflineRenderStepResult::Failed;
            }
        }
        return OfflineRenderStepResult::Waiting;
    }

    mRenderWaitFrames = 0;

    // The download is acknowledged by finishDownload(). The driver calls that
    // method before advancing to the next plan sample, so this state remains
    // pending until the writer has received the matching pixels.
    return OfflineRenderStepResult::Waiting;
}

bool OfflineRenderBoundary::beginDrain() {
    if (mState != OfflineRenderBoundaryState::Ready || mPendingFrame || mCompletedFrameTicket) return false;
    clearClockSample();
    mReplay.endExportTimeline();
    mState = OfflineRenderBoundaryState::Draining;
    return true;
}

bool OfflineRenderBoundary::isDrained() {
    if (mState != OfflineRenderBoundaryState::Draining) return false;
    mExecutor.pollCapture();
    return mDownloads.isEmpty();
}

std::optional<functions::render::CapturedFrame> OfflineRenderBoundary::finishDownload() {
    mExecutor.pollCapture();
    auto frame = mDownloads.finishDownload();
    if (!frame) return std::nullopt;

    // Once the GPU readback is available, release the sample immediately. This
    // acts as a waiting FIFO: the writer may consume the frame and
    // the next replay sample can be prepared on the following driver pass.
    if (!mPendingFrame) {
        if (mState == OfflineRenderBoundaryState::Draining) return frame;
        fault(OfflineRenderBoundaryError::CaptureFailed, "The GPU download completed outside its render sample");
        return std::nullopt;
    }

    if (!ticketsEqual(mPendingFrame->ticket, frame->ticket)) {
        fault(OfflineRenderBoundaryError::CaptureFailed, "The GPU download ticket does not match its render sample");
        return std::nullopt;
    }
    if (!mClockToken || !wasOfflineRenderClockSampleApplied(*mClockToken)) {
        fault(
            OfflineRenderBoundaryError::ClockUnavailable,
            "The captured frame completed before its fractional clock was applied"
        );
        return std::nullopt;
    }

    mExecutor.completeSample(mPendingFrame->ticket);
    clearClockSample();
    mLastSubmittedFrame = mPendingFrame;
    mPendingFrame.reset();
    mCaptureRetryCount   = 0;
    mCompletedFrameTicket = frame->ticket;
    mState                 = OfflineRenderBoundaryState::Ready;
    return frame;
}

OfflineRenderBoundaryStatus OfflineRenderBoundary::status() {
    OfflineRenderBoundaryStatus result;
    result.state                 = mState;
    result.error                 = mError;
    result.message               = mMessage;
    result.downloads             = mDownloads.status();
    result.executor              = mExecutor.status();
    result.warmupFramesRemaining = mWarmupFramesRemaining;
    if (result.state != OfflineRenderBoundaryState::Faulted) {
        if (result.downloads.state == FrameDownloadQueueState::Faulted) {
            if (recoverDownloadFailure(result.downloads)) {
                result.downloads = mDownloads.status();
                result.state     = mState;
            } else {
                fault(
                    OfflineRenderBoundaryError::CaptureFailed,
                    result.downloads.message.empty() ? "The renderer frame download failed"
                                                     : result.downloads.message
                );
            }
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
    auto const sample =
        functions::render::ReplaySampleTime::fromRational(frame.replayTickNumerator, frame.replayTickDenominator);
    if (!sample) return 0;
    auto const clamped = std::clamp<int64_t>(sample->requiredAppliedTick(), 0, mMaximumReplayTick);
    return clamped > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(clamped);
}

std::optional<OfflineRenderClockSample> OfflineRenderBoundary::clockSample(ExportFramePlan const& frame) const {
    auto const replayTime =
        functions::render::ReplaySampleTime::fromRational(frame.replayTickNumerator, frame.replayTickDenominator);
    if (!replayTime) return std::nullopt;

    long double const current = replayTime->value();
    if (current < 0.0L || current > static_cast<long double>(mMaximumReplayTick)) return std::nullopt;

    long double delta             = 0.0L;
    int64_t     previousWholeTick = frame.replayTickNumerator / frame.replayTickDenominator;
    if (mLastSubmittedFrame) {
        delta = current
              - static_cast<long double>(mLastSubmittedFrame->replayTickNumerator)
                    / static_cast<long double>(mLastSubmittedFrame->replayTickDenominator);
        previousWholeTick = mLastSubmittedFrame->replayTickNumerator / mLastSubmittedFrame->replayTickDenominator;
    }
    if (delta < 0.0L || delta > static_cast<long double>(std::numeric_limits<float>::max())) return std::nullopt;

    int64_t const currentWholeTick = frame.replayTickNumerator / frame.replayTickDenominator;
    int64_t const wholeTicks       = currentWholeTick - previousWholeTick;
    if (wholeTicks < 0 || wholeTicks > std::numeric_limits<int>::max()) return std::nullopt;

    return OfflineRenderClockSample{
        *replayTime,
        static_cast<float>(delta),
        static_cast<int>(wholeTicks),
    };
}

OfflineRenderStepResult OfflineRenderBoundary::advanceWarmup(ExportFramePlan const& frame) {
    if (mWarmupFramesRemaining == 0) {
        mState = OfflineRenderBoundaryState::PreparingReplay;
        return OfflineRenderStepResult::Waiting;
    }
    if (!mClockToken && !publishClockSample(frame)) return OfflineRenderStepResult::Failed;

    switch (mExecutor.executeWarmup(frame, *mClockToken)) {
    case OfflineRenderFrameExecutionResult::Waiting:
        return OfflineRenderStepResult::Waiting;
    case OfflineRenderFrameExecutionResult::Failed: {
        auto const executorStatus = mExecutor.status();
        fault(
            OfflineRenderBoundaryError::CaptureUnavailable,
            executorStatus.message.empty() ? "The offline warm-up render failed" : executorStatus.message
        );
        return OfflineRenderStepResult::Failed;
    }
    case OfflineRenderFrameExecutionResult::Executed:
        break;
    }

    if (!wasOfflineRenderClockSampleApplied(*mClockToken)) {
        fault(OfflineRenderBoundaryError::ClockUnavailable, "The warm-up render did not apply its clock sample");
        return OfflineRenderStepResult::Failed;
    }

    mExecutor.completeWarmup();
    clearClockSample();
    --mWarmupFramesRemaining;
    if (mWarmupFramesRemaining == 0) mState = OfflineRenderBoundaryState::PreparingReplay;
    return OfflineRenderStepResult::Waiting;
}

bool OfflineRenderBoundary::recoverDownloadFailure(FrameDownloadQueueStatus const& status) {
    if (status.state != FrameDownloadQueueState::Faulted
        || status.error != functions::render::FrameTapError::MapFailed || !mPendingFrame || mCaptureCapacity == 0
        || mCaptureRetryCount >= MaxCaptureRetries) {
        return false;
    }

    ++mCaptureRetryCount;
    Playback::getInstance().getSelf().getLogger().warn(
        "Retrying D3D frame readback for export frame {} ({}/{}) after: {}",
        mPendingFrame->ticket.frameIndex,
        mCaptureRetryCount,
        MaxCaptureRetries,
        status.message.empty() ? "map failure" : status.message
    );

    mExecutor.completeSample(mPendingFrame->ticket);
    clearClockSample();
    if (!mDownloads.open(mCaptureCapacity)) return false;

    mRenderWaitFrames = 0;
    mState            = OfflineRenderBoundaryState::PreparingReplay;
    return true;
}

bool OfflineRenderBoundary::publishClockSample(ExportFramePlan const& frame) {
    auto const sample = clockSample(frame);
    if (!sample) {
        fault(OfflineRenderBoundaryError::InvalidFrame, "The offline render clock sample is invalid");
        return false;
    }

    OfflineRenderClockToken token;
    switch (publishOfflineRenderClockSample(*sample, token)) {
    case OfflineRenderClockPublishResult::Published:
        mClockToken = token;
        return true;
    case OfflineRenderClockPublishResult::Unavailable:
        fault(OfflineRenderBoundaryError::ClockUnavailable, "The fractional render clock is unavailable");
        return false;
    case OfflineRenderClockPublishResult::Busy:
        fault(OfflineRenderBoundaryError::ClockUnavailable, "The fractional render clock is already in use");
        return false;
    case OfflineRenderClockPublishResult::InvalidSample:
        fault(OfflineRenderBoundaryError::InvalidFrame, "The fractional render clock rejected the frame sample");
        return false;
    }
    return false;
}

void OfflineRenderBoundary::clearClockSample() {
    if (!mClockToken) return;
    clearOfflineRenderClockSample(*mClockToken);
    mClockToken.reset();
}

void OfflineRenderBoundary::fault(OfflineRenderBoundaryError error, std::string message) {
    setOfflineRenderActivityActive(false);
    clearClockSample();
    mReplayTickToken.reset();
    if (mTickGateOpen) {
        functions::endOfflineReplayTickGate();
        mTickGateOpen = false;
    }
    mReplay.endExportTimeline();
    mDownloads.cancel();
    mExecutor.close();
    mPendingFrame.reset();
    mCompletedFrameTicket.reset();
    mCaptureCapacity       = 0;
    mCaptureRetryCount     = 0;
    mWarmupFramesRemaining = 0;
    mRenderWaitFrames      = 0;
    mState                 = OfflineRenderBoundaryState::Faulted;
    mError                 = error;
    mMessage               = std::move(message);
}

} // namespace playback::editor::exporting
