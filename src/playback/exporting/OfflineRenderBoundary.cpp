#include "OfflineRenderBoundary.h"

#include "ExportActivity.h"
#include "playback/Playback.h"
#include "playback/keyframe/CameraTimelineRegistry.h"
#include "playback/replay/ReplaySession.h"
#include "playback/visuals/ReplaySampleTime.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace playback::exporting {

namespace {

constexpr uint32_t MaxCaptureRetries       = 2;
constexpr uint32_t MaxReplayTickRecoveries = 2;
constexpr auto     ReplayTickWaitTimeout   = std::chrono::seconds{2};
constexpr auto     RenderWaitTimeout       = std::chrono::seconds{30};
constexpr auto     RenderWaitLogInterval   = std::chrono::seconds{2};
constexpr auto     WarmupSceneTimeout      = std::chrono::seconds{30};
constexpr uint32_t StableWarmupFrames      = 3;
constexpr uint32_t ClearRetryWarmupFrames  = 3;

bool ticketsEqual(visuals::FrameTicket const& left, visuals::FrameTicket const& right) {
    return left.frameIndex == right.frameIndex && left.ptsNumerator == right.ptsNumerator
        && left.ptsDenominator == right.ptsDenominator;
}

} // namespace

OfflineRenderBoundary::OfflineRenderBoundary(replay::ReplaySession& replay, visuals::FrameTap& frameTap)
: mReplay(replay),
  mDownloads(frameTap) {}

OfflineRenderBoundary::~OfflineRenderBoundary() { close(); }

bool OfflineRenderBoundary::open(
    uint32_t                                     capacity,
    ExportSettings const&                        settings,
    state::editing::model::EditorStateExt const& project,
    std::optional<std::string>                   cameraFallback
) {
    close();
    setOfflineRenderActivityActive(false);
    if (!isOfflineRenderClockInstalled()) return false;
    if (!mExecutor.open(settings, project, std::move(cameraFallback))) return false;
    if (!mDownloads.open(capacity)) {
        mExecutor.close();
        return false;
    }
    mCaptureCapacity = capacity;

    mMaximumReplayTick        = std::max<int64_t>(0, settings.endTick);
    auto const maximumIntTick = std::min<int64_t>(mMaximumReplayTick, std::numeric_limits<int>::max());
    auto const startTick      = std::clamp<int64_t>(settings.startTick, 0, maximumIntTick);
    visuals::ReplaySampleTime const startTime{startTick, 1};
    auto const cameraSample = keyframe::sampleCameraTimeline(keyframe::CameraTimelineSource::Export, startTime);
    std::optional<replay::ReplayCameraViewpoint> cameraViewpoint;
    if (cameraSample) {
        cameraViewpoint = replay::ReplayCameraViewpoint{
            cameraSample->state.x,
            cameraSample->state.y,
            cameraSample->state.z,
            cameraSample->state.pitch,
            cameraSample->state.yaw,
        };
    }
    mReplay.setExportCameraViewpoint(cameraViewpoint);
    if (!mReplay.beginExportTimeline(static_cast<int>(startTick))) {
        mReplay.endExportTimeline();
        mDownloads.close();
        mExecutor.close();
        mMaximumReplayTick = 0;
        mCaptureCapacity   = 0;
        return false;
    }

    mTimelineInitialized           = false;
    mInitializationTickObserved    = false;
    mWarmupFramesRemaining         = settings.warmupFrames;
    mWarmupStableFrames            = 0;
    mWarmupStartedAt               = {};
    mWarmupLastLoggedAt            = {};
    mReplayTickRecoveryCount       = 0;
    mReplayTickRequestedAt         = {};
    mTickGateSuspendedForDimension = false;
    mCameraSampleActive            = false;
    mState                         = OfflineRenderBoundaryState::Ready;
    mError                         = OfflineRenderBoundaryError::None;
    mMessage.clear();
    return true;
}

void OfflineRenderBoundary::close() {
    setOfflineRenderActivityActive(false);
    clearClockSample();
    mReplayTickToken.reset();
    if (mTickGateOpen) {
        runtime::endOfflineReplayTickGate();
        mTickGateOpen = false;
    }
    mReplay.endExportTimeline();
    mDownloads.close();
    mExecutor.close();
    mPendingFrame.reset();
    mLastSubmittedFrame.reset();
    mCompletedFrameTicket.reset();
    mMaximumReplayTick             = 0;
    mCaptureCapacity               = 0;
    mCaptureRetryCount             = 0;
    mReplayTickRecoveryCount       = 0;
    mWarmupFramesRemaining         = 0;
    mWarmupStableFrames            = 0;
    mRenderWaitPolls               = 0;
    mRenderWaitStartedAt           = {};
    mRenderWaitLastLoggedAt        = {};
    mReplayTickRequestedAt         = {};
    mWarmupStartedAt               = {};
    mWarmupLastLoggedAt            = {};
    mTickGateSuspendedForDimension = false;
    mCameraSampleActive            = false;
    mTimelineInitialized           = false;
    mInitializationTickObserved    = false;
    mState                         = OfflineRenderBoundaryState::Closed;
    mError                         = OfflineRenderBoundaryError::None;
    mMessage.clear();
}

void OfflineRenderBoundary::cancel() {
    setOfflineRenderActivityActive(false);
    clearClockSample();
    mReplayTickToken.reset();
    if (mTickGateOpen) {
        runtime::endOfflineReplayTickGate();
        mTickGateOpen = false;
    }
    mReplay.endExportTimeline();
    mDownloads.cancel();
    mExecutor.close();
    mPendingFrame.reset();
    mLastSubmittedFrame.reset();
    mCompletedFrameTicket.reset();
    mMaximumReplayTick             = 0;
    mCaptureCapacity               = 0;
    mCaptureRetryCount             = 0;
    mReplayTickRecoveryCount       = 0;
    mWarmupFramesRemaining         = 0;
    mWarmupStableFrames            = 0;
    mRenderWaitPolls               = 0;
    mRenderWaitStartedAt           = {};
    mRenderWaitLastLoggedAt        = {};
    mReplayTickRequestedAt         = {};
    mWarmupStartedAt               = {};
    mWarmupLastLoggedAt            = {};
    mTickGateSuspendedForDimension = false;
    mCameraSampleActive            = false;
    mTimelineInitialized           = false;
    mInitializationTickObserved    = false;
    mState                         = OfflineRenderBoundaryState::Cancelled;
    mError                         = OfflineRenderBoundaryError::None;
    mMessage                       = "Offline rendering was cancelled";
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
            fault(
                OfflineRenderBoundaryError::InvalidFrame,
                "The completed render sample was not acknowledged in order"
            );
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
        mState        = !mTimelineInitialized ? OfflineRenderBoundaryState::InitializingReplay
                                              : OfflineRenderBoundaryState::WarmingUp;
    }

    if (mTickGateOpen && mReplay.isDimensionTransitionPending()
        && (mState == OfflineRenderBoundaryState::InitializingReplay
            || mState == OfflineRenderBoundaryState::PreparingReplay)) {
        mReplayTickToken.reset();
        mReplayTickRecoveryCount = 0;
        mReplayTickRequestedAt   = {};
        runtime::endOfflineReplayTickGate();
        mTickGateOpen                  = false;
        mTickGateSuspendedForDimension = true;
        mWarmupStableFrames            = 0;
        mWarmupStartedAt               = {};
        mWarmupLastLoggedAt            = {};
        setOfflineRenderActivityActive(false);
        Playback::getInstance().getSelf().getLogger().info(
            "Offline replay tick gate suspended for a native dimension transition at replay tick {}",
            mReplay.getAppliedReplayTick()
        );
        return OfflineRenderStepResult::Waiting;
    }

    if (!mTickGateOpen
        && (mState == OfflineRenderBoundaryState::InitializingReplay
            || mState == OfflineRenderBoundaryState::PreparingReplay)) {
        switch (mReplay.prepareExportTick(targetTick(*mPendingFrame))) {
        case replay::ReplayExportTickState::Unavailable:
            fault(OfflineRenderBoundaryError::ReplayUnavailable, "The replay became unavailable during export");
            return OfflineRenderStepResult::Failed;
        case replay::ReplayExportTickState::Invalid:
            fault(
                OfflineRenderBoundaryError::InvalidFrame,
                "The export frame moved backwards or changed its initialization tick"
            );
            return OfflineRenderStepResult::Failed;
        case replay::ReplayExportTickState::Failed:
            fault(OfflineRenderBoundaryError::ReplayFailed, "The replay failed while preparing an export frame");
            return OfflineRenderStepResult::Failed;
        case replay::ReplayExportTickState::Waiting:
            return OfflineRenderStepResult::Waiting;
        case replay::ReplayExportTickState::Ready:
            break;
        }

        if (mTickGateSuspendedForDimension) {
            updateExportCamera(*mPendingFrame);
            auto const now = std::chrono::steady_clock::now();
            if (mWarmupStartedAt == std::chrono::steady_clock::time_point{}) mWarmupStartedAt = now;
            if (!mExecutor.isUiStable()) {
                if (now - mWarmupStartedAt > WarmupSceneTimeout) {
                    fault(
                        OfflineRenderBoundaryError::ReplayUnavailable,
                        "The native dimension loading screen did not become stable within 30 seconds"
                    );
                    return OfflineRenderStepResult::Failed;
                }
                return OfflineRenderStepResult::Waiting;
            }
            mWarmupStartedAt    = {};
            mWarmupLastLoggedAt = {};
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

        if (!runtime::beginOfflineReplayTickGate()) {
            fault(OfflineRenderBoundaryError::TickUnavailable, "The offline replay tick gate is unavailable");
            return OfflineRenderStepResult::Failed;
        }
        mTickGateOpen = true;
        if (mTickGateSuspendedForDimension) {
            Playback::getInstance().getSelf().getLogger().info(
                "Offline replay tick gate resumed after the native dimension transition at replay tick {}",
                mReplay.getAppliedReplayTick()
            );
            mTickGateSuspendedForDimension = false;
        }
        setOfflineRenderActivityActive(true);
        mState = OfflineRenderBoundaryState::PreparingReplay;
    }

    if (mState == OfflineRenderBoundaryState::InitializingReplay
        || mState == OfflineRenderBoundaryState::PreparingReplay) {
        if (mReplayTickToken) {
            if (!runtime::wasOfflineReplayTickCompleted(*mReplayTickToken)) {
                auto const now = std::chrono::steady_clock::now();
                if (mReplayTickRequestedAt == std::chrono::steady_clock::time_point{}) {
                    mReplayTickRequestedAt = now;
                }
                if (now - mReplayTickRequestedAt < ReplayTickWaitTimeout) {
                    return OfflineRenderStepResult::Waiting;
                }
                if (mReplayTickRecoveryCount >= MaxReplayTickRecoveries) {
                    fault(
                        OfflineRenderBoundaryError::TickUnavailable,
                        "The offline replay tick did not execute after two gate recoveries"
                    );
                    return OfflineRenderStepResult::Failed;
                }

                ++mReplayTickRecoveryCount;
                Playback::getInstance().getSelf().getLogger().warn(
                    "Recovering stalled offline replay tick token {} for export frame {} at replay tick {} ({}/{})",
                    mReplayTickToken->id,
                    mPendingFrame->ticket.frameIndex,
                    mReplay.getAppliedReplayTick(),
                    mReplayTickRecoveryCount,
                    MaxReplayTickRecoveries
                );
                mReplayTickToken.reset();
                mReplayTickRequestedAt = {};
                runtime::endOfflineReplayTickGate();
                mTickGateOpen = false;
                if (!runtime::beginOfflineReplayTickGate()) {
                    fault(
                        OfflineRenderBoundaryError::TickUnavailable,
                        "Unable to recover the offline replay tick gate"
                    );
                    return OfflineRenderStepResult::Failed;
                }
                mTickGateOpen = true;
                return OfflineRenderStepResult::Waiting;
            }

            auto const completion = runtime::getOfflineReplayTickCompletion(*mReplayTickToken);
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
            mReplayTickRecoveryCount = 0;
            mReplayTickRequestedAt   = {};
        }

        auto requestReplayTick = [&]() -> OfflineRenderStepResult {
            runtime::OfflineReplayTickToken token;
            switch (runtime::requestOfflineReplayTick(token)) {
            case runtime::OfflineReplayTickRequestResult::Requested:
                mReplayTickToken       = token;
                mReplayTickRequestedAt = std::chrono::steady_clock::now();
                return OfflineRenderStepResult::Waiting;
            case runtime::OfflineReplayTickRequestResult::Unavailable:
                fault(OfflineRenderBoundaryError::TickUnavailable, "The offline replay tick gate is unavailable");
                return OfflineRenderStepResult::Failed;
            case runtime::OfflineReplayTickRequestResult::Busy:
                fault(OfflineRenderBoundaryError::TickUnavailable, "The offline replay tick gate is already in use");
                return OfflineRenderStepResult::Failed;
            }
            return OfflineRenderStepResult::Waiting;
        };

        switch (mReplay.prepareExportTick(targetTick(*mPendingFrame))) {
        case replay::ReplayExportTickState::Unavailable:
            fault(OfflineRenderBoundaryError::ReplayUnavailable, "The replay became unavailable during export");
            return OfflineRenderStepResult::Failed;
        case replay::ReplayExportTickState::Invalid:
            fault(
                OfflineRenderBoundaryError::InvalidFrame,
                "The export frame moved backwards or changed its initialization tick"
            );
            return OfflineRenderStepResult::Failed;
        case replay::ReplayExportTickState::Failed:
            fault(OfflineRenderBoundaryError::ReplayFailed, "The replay failed while preparing an export frame");
            return OfflineRenderStepResult::Failed;
        case replay::ReplayExportTickState::Waiting:
            return requestReplayTick();
        case replay::ReplayExportTickState::Ready:
            break;
        }

        updateExportCamera(*mPendingFrame);

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
            mState                      = OfflineRenderBoundaryState::WarmingUp;
        }

        if (!warmupComplete()) {
            mState = OfflineRenderBoundaryState::WarmingUp;
            return advanceWarmup(*mPendingFrame);
        }

        if (!mClockToken) {
            if (!publishClockSample(*mPendingFrame)) return OfflineRenderStepResult::Failed;
        }

        switch (mDownloads.requestDownload(mPendingFrame->ticket)) {
        case FrameDownloadRequestResult::Requested:
            mState                  = OfflineRenderBoundaryState::AwaitingDownload;
            mRenderWaitPolls        = 0;
            mRenderWaitStartedAt    = std::chrono::steady_clock::now();
            mRenderWaitLastLoggedAt = {};
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
            auto const now = std::chrono::steady_clock::now();
            if (mRenderWaitStartedAt == std::chrono::steady_clock::time_point{}) mRenderWaitStartedAt = now;
            ++mRenderWaitPolls;
            if (mRenderWaitLastLoggedAt == std::chrono::steady_clock::time_point{}
                || now - mRenderWaitLastLoggedAt >= RenderWaitLogInterval) {
                auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - mRenderWaitStartedAt);
                Playback::getInstance().getSelf().getLogger().debug(
                    "Offline render waiting for scene submit/FrameTap (elapsedMs={}, polls={}, frame={}, tapArmed={}, "
                    "tapInFlight={}, renderSize={}x{})",
                    elapsed.count(),
                    mRenderWaitPolls,
                    mPendingFrame->ticket.frameIndex,
                    downloadStatus.tap.armed,
                    downloadStatus.tap.inFlightFrames,
                    mExecutor.status().renderWidth,
                    mExecutor.status().renderHeight
                );
                mRenderWaitLastLoggedAt = now;
            }
            if (now - mRenderWaitStartedAt > RenderWaitTimeout) {
                fault(
                    OfflineRenderBoundaryError::CaptureFailed,
                    "The matching FrameTap capture did not start within 30 seconds"
                );
                return OfflineRenderStepResult::Failed;
            }
        }
        return OfflineRenderStepResult::Waiting;
    }

    mRenderWaitPolls        = 0;
    mRenderWaitStartedAt    = {};
    mRenderWaitLastLoggedAt = {};

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

bool OfflineRenderBoundary::retryCompletedFrame(visuals::FrameTicket const& ticket) {
    if (mState != OfflineRenderBoundaryState::Ready || mPendingFrame || !mCompletedFrameTicket || !mLastSubmittedFrame
        || !ticketsEqual(*mCompletedFrameTicket, ticket) || !ticketsEqual(mLastSubmittedFrame->ticket, ticket)
        || !mDownloads.isEmpty()) {
        return false;
    }
    mCompletedFrameTicket.reset();
    mWarmupFramesRemaining = std::max(mWarmupFramesRemaining, ClearRetryWarmupFrames);
    mWarmupStableFrames    = 0;
    mWarmupStartedAt       = {};
    mWarmupLastLoggedAt    = {};
    mState                 = OfflineRenderBoundaryState::WarmingUp;
    return true;
}

std::optional<visuals::CapturedFrame> OfflineRenderBoundary::finishDownload() {
    mExecutor.pollCapture();
    auto frame = mDownloads.finishDownload();
    if (!frame) return std::nullopt;

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
    mCaptureRetryCount    = 0;
    mCompletedFrameTicket = frame->ticket;
    mState                = OfflineRenderBoundaryState::Ready;
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
    result.warmupStableFrames    = mWarmupStableFrames;
    if (result.state != OfflineRenderBoundaryState::Faulted) {
        if (result.downloads.state == FrameDownloadQueueState::Faulted) {
            if (recoverDownloadFailure(result.downloads)) {
                result.downloads = mDownloads.status();
                result.state     = mState;
            } else {
                fault(
                    OfflineRenderBoundaryError::CaptureFailed,
                    result.downloads.message.empty() ? "The renderer frame download failed" : result.downloads.message
                );
            }
        } else if (
            result.state != OfflineRenderBoundaryState::Closed && result.state != OfflineRenderBoundaryState::Cancelled
            && (result.downloads.state == FrameDownloadQueueState::Closed
                || result.downloads.state == FrameDownloadQueueState::Cancelled)
        ) {
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
    auto const sample = visuals::ReplaySampleTime::fromRational(frame.replayTickNumerator, frame.replayTickDenominator);
    if (!sample) return 0;
    auto const clamped = std::clamp<int64_t>(sample->requiredAppliedTick(), 0, mMaximumReplayTick);
    return clamped > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(clamped);
}

std::optional<OfflineRenderClockSample> OfflineRenderBoundary::clockSample(ExportFramePlan const& frame) const {
    auto const replayTime =
        visuals::ReplaySampleTime::fromRational(frame.replayTickNumerator, frame.replayTickDenominator);
    if (!replayTime) return std::nullopt;

    long double const current = replayTime->value();
    if (current < 0.0L || current > static_cast<long double>(mMaximumReplayTick)) return std::nullopt;

    long double delta             = 0.0L;
    int64_t     previousWholeTick = frame.replayTickNumerator / frame.replayTickDenominator;
    if (mLastSubmittedFrame) {
        delta             = current
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
        frame.ticket.frameIndex,
    };
}

void OfflineRenderBoundary::updateExportCamera(ExportFramePlan const& frame) {
    mCameraSampleActive = false;
    auto const sample   = clockSample(frame);
    if (!sample) {
        mReplay.setExportCameraViewpoint(std::nullopt);
        return;
    }

    auto const cameraSample =
        keyframe::sampleCameraTimeline(keyframe::CameraTimelineSource::Export, sample->replayTime);
    std::optional<replay::ReplayCameraViewpoint> viewpoint;
    if (cameraSample) {
        mCameraSampleActive = true;
        viewpoint           = replay::ReplayCameraViewpoint{
            cameraSample->state.x,
            cameraSample->state.y,
            cameraSample->state.z,
            cameraSample->state.pitch,
            cameraSample->state.yaw,
        };
    }
    mReplay.setExportCameraViewpoint(viewpoint);
    if (viewpoint) mReplay.updateExportObserver(*viewpoint);
    if (warmupComplete() && mCameraSampleActive && !mReplay.getSceneReadiness().ready()) {
        mWarmupStableFrames = 0;
        mWarmupStartedAt    = {};
        mWarmupLastLoggedAt = {};
    }
}

OfflineRenderStepResult OfflineRenderBoundary::advanceWarmup(ExportFramePlan const& frame) {
    if (warmupComplete()) {
        mState = OfflineRenderBoundaryState::PreparingReplay;
        return OfflineRenderStepResult::Waiting;
    }
    auto const warmupNow = std::chrono::steady_clock::now();
    if (mWarmupStartedAt == std::chrono::steady_clock::time_point{}) mWarmupStartedAt = warmupNow;
    if (!mClockToken && !publishClockSample(frame)) return OfflineRenderStepResult::Failed;

    switch (mExecutor.executeWarmup(*mClockToken)) {
    case OfflineRenderFrameExecutionResult::Waiting: {
        auto const now = std::chrono::steady_clock::now();
        if (mRenderWaitStartedAt == std::chrono::steady_clock::time_point{}) mRenderWaitStartedAt = now;
        ++mRenderWaitPolls;
        if (mRenderWaitLastLoggedAt == std::chrono::steady_clock::time_point{}
            || now - mRenderWaitLastLoggedAt >= RenderWaitLogInterval) {
            auto const elapsed  = std::chrono::duration_cast<std::chrono::milliseconds>(now - mRenderWaitStartedAt);
            auto const executor = mExecutor.status();
            Playback::getInstance().getSelf().getLogger().debug(
                "Offline warm-up waiting for render completion (elapsedMs={}, polls={}, remaining={}, frame={}, "
                "clockApplied={}, clockCompleted={}, uiStable={}, stableFrames={}/{})",
                elapsed.count(),
                mRenderWaitPolls,
                mWarmupFramesRemaining,
                frame.ticket.frameIndex,
                wasOfflineRenderClockSampleApplied(*mClockToken),
                wasOfflineRenderClockSampleCompleted(*mClockToken),
                executor.uiStable,
                mWarmupStableFrames,
                StableWarmupFrames
            );
            mRenderWaitLastLoggedAt = now;
        }
        if (now - mRenderWaitStartedAt > RenderWaitTimeout) {
            fault(
                OfflineRenderBoundaryError::CaptureFailed,
                "The matching warm-up scene submission did not complete within 30 seconds"
            );
            return OfflineRenderStepResult::Failed;
        }
    }
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

    mRenderWaitPolls        = 0;
    mRenderWaitStartedAt    = {};
    mRenderWaitLastLoggedAt = {};
    mExecutor.completeWarmup();
    auto const                                  executor   = mExecutor.status();
    bool                                        sceneReady = true;
    std::optional<replay::ReplaySceneReadiness> scene;
    if (mCameraSampleActive) {
        scene      = mReplay.getSceneReadiness();
        sceneReady = scene->ready();
    }
    clearClockSample();
    if (mWarmupFramesRemaining != 0) --mWarmupFramesRemaining;
    bool const stableFrame = executor.uiStable && sceneReady;
    mWarmupStableFrames    = stableFrame ? mWarmupStableFrames + 1 : 0;

    auto const now = std::chrono::steady_clock::now();
    if (!stableFrame
        && (mWarmupLastLoggedAt == std::chrono::steady_clock::time_point{}
            || now - mWarmupLastLoggedAt >= RenderWaitLogInterval)) {
        auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - mWarmupStartedAt);
        if (scene) {
            Playback::getInstance().getSelf().getLogger().warn(
                "Offline camera scene is not ready during warm-up (elapsedMs={}, frame={}, chunk=({}, {}), "
                "replayReady={}, dimensionPending={}, snapshotPending={}, injectionPending={}, present={}, "
                "empty={}, loaded={}, loadState={}, recorded={}/{}, ready={}/{}, presentCount={}, emptyCount={}, "
                "uiStable={}, remaining={}, stableFrames={}/{})",
                elapsed.count(),
                frame.ticket.frameIndex,
                scene->chunkX,
                scene->chunkZ,
                scene->replayReady,
                scene->dimensionTransitionPending,
                scene->snapshotPending,
                scene->chunkInjectionPending,
                scene->chunkPresent,
                scene->chunkEmpty,
                scene->chunkLoaded,
                scene->chunkLoadState,
                scene->recordedChunkCount,
                scene->requiredChunkCount,
                scene->readyChunkCount,
                scene->requiredChunkCount,
                scene->presentChunkCount,
                scene->emptyChunkCount,
                executor.uiStable,
                mWarmupFramesRemaining,
                mWarmupStableFrames,
                StableWarmupFrames
            );
        } else {
            Playback::getInstance().getSelf().getLogger().warn(
                "Offline scene is not stable during warm-up (elapsedMs={}, frame={}, uiStable={}, remaining={}, "
                "stableFrames={}/{})",
                elapsed.count(),
                frame.ticket.frameIndex,
                executor.uiStable,
                mWarmupFramesRemaining,
                mWarmupStableFrames,
                StableWarmupFrames
            );
        }
        mWarmupLastLoggedAt = now;
    }

    if (mWarmupFramesRemaining == 0 && !stableFrame && now - mWarmupStartedAt > WarmupSceneTimeout) {
        fault(
            OfflineRenderBoundaryError::ReplayUnavailable,
            mCameraSampleActive ? "The export camera chunk did not become ready during warm-up"
                                : "The export scene did not become stable during warm-up"
        );
        return OfflineRenderStepResult::Failed;
    }
    if (warmupComplete()) mState = OfflineRenderBoundaryState::PreparingReplay;
    return OfflineRenderStepResult::Waiting;
}

bool OfflineRenderBoundary::warmupComplete() const {
    return mWarmupFramesRemaining == 0 && mWarmupStableFrames >= StableWarmupFrames;
}

bool OfflineRenderBoundary::recoverDownloadFailure(FrameDownloadQueueStatus const& status) {
    if (status.state != FrameDownloadQueueState::Faulted || status.error != visuals::FrameTapError::MapFailed
        || !mPendingFrame || mCaptureCapacity == 0 || mCaptureRetryCount >= MaxCaptureRetries) {
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

    mRenderWaitPolls        = 0;
    mRenderWaitStartedAt    = {};
    mRenderWaitLastLoggedAt = {};
    mState                  = OfflineRenderBoundaryState::PreparingReplay;
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
        runtime::endOfflineReplayTickGate();
        mTickGateOpen = false;
    }
    mReplay.endExportTimeline();
    mDownloads.cancel();
    mExecutor.close();
    mPendingFrame.reset();
    mCompletedFrameTicket.reset();
    mCaptureCapacity               = 0;
    mCaptureRetryCount             = 0;
    mReplayTickRecoveryCount       = 0;
    mWarmupFramesRemaining         = 0;
    mWarmupStableFrames            = 0;
    mRenderWaitPolls               = 0;
    mRenderWaitStartedAt           = {};
    mRenderWaitLastLoggedAt        = {};
    mReplayTickRequestedAt         = {};
    mWarmupStartedAt               = {};
    mWarmupLastLoggedAt            = {};
    mTickGateSuspendedForDimension = false;
    mState                         = OfflineRenderBoundaryState::Faulted;
    mError                         = error;
    mMessage                       = std::move(message);
}

} // namespace playback::exporting
