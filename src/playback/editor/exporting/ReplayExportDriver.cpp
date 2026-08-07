#include "ReplayExportDriver.h"

#include "ExportActivity.h"

#include "playback/Playback.h"
#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/functions/replay/ReplaySession.h"
#include "playback/screen/IdleDetectionHooks.h"

#include <utility>

namespace playback::editor::exporting {

namespace {

constexpr uint32_t ExportCaptureCapacity = 4;

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

} // namespace

ReplayExportDriver::ReplayExportDriver(ExportCoordinator& coordinator, functions::ReplaySession& replay)
: mCoordinator(coordinator),
  mReplay(replay) {}

ReplayExportDriver::~ReplayExportDriver() { reset(); }

void ReplayExportDriver::setFrameTap(functions::render::FrameTap* frameTap) {
    if (mRenderBoundary && isActive()) cancel();
    mRenderBoundary.reset();
    if (frameTap) mRenderBoundary = std::make_unique<OfflineRenderBoundary>(mReplay, *frameTap);
}

bool ReplayExportDriver::start(ExportSettings settings, editing::model::EditorStateExt const& project) {
    if (isActive()) return false;
    if (!screen::isIdleDetectionGuardInstalled()) {
        mCoordinator.fail(
            ExportError::CaptureUnavailable,
            "Minecraft's idle detection guard is unavailable; export was blocked to prevent invalid frames"
        );
        mPhase = Phase::Faulted;
        return false;
    }
    if (!isOfflineRenderClockInstalled()) {
        mCoordinator.fail(
            ExportError::CaptureUnavailable,
            "The fractional render clock is unavailable; export was blocked to prevent integer-only frames"
        );
        mPhase = Phase::Faulted;
        return false;
    }
    if (!mRenderBoundary) {
        mCoordinator.fail(ExportError::CaptureUnavailable, "The offline renderer boundary is unavailable");
        mPhase = Phase::Faulted;
        return false;
    }
    if (!mReplay.isActive()) {
        mCoordinator.fail(ExportError::ReplayUnavailable, "A replay must be active before video export can start");
        mPhase = Phase::Faulted;
        return false;
    }

    mPreviousPaused = mReplay.isPaused();
    if (!mReplay.setPaused(true)) {
        mCoordinator.fail(ExportError::ReplayUnavailable, "Unable to pause the replay for export");
        mPhase = Phase::Faulted;
        return false;
    }
    mRestorePaused = true;

    if (!mRenderBoundary->open(ExportCaptureCapacity, settings.endTick)) {
        restoreReplayState();
        mCoordinator.fail(ExportError::CaptureUnavailable, "The renderer frame download queue is busy");
        mPhase = Phase::Faulted;
        return false;
    }
    if (!mCoordinator.start(std::move(settings), project)) {
        closeCapture(true);
        restoreReplayState();
        mPhase = Phase::Faulted;
        return false;
    }

    mPlan = mCoordinator.plan();
    if (!mPlan) {
        fail(ExportError::InvalidSettings, "The export plan was not retained by the coordinator");
        return false;
    }
    mReadyFrames.clear();
    mNextFrameIndex = 0;
    setExportActivityActive(true);
    setOfflineRenderActivityActive(true);
    mPhase = Phase::Rendering;
    getLogger().info(
        "Video export started: output={}, format={}, frames={}, ticks={}-{}, fps={}/{}, replayTick={}",
        mPlan->outputPath,
        static_cast<int>(mPlan->settings.format),
        mPlan->frameCount,
        mPlan->settings.startTick,
        mPlan->settings.endTick,
        mPlan->settings.frameRate.numerator,
        mPlan->settings.frameRate.denominator,
        mReplay.getCurrentTick()
    );
    return true;
}

void ReplayExportDriver::tick() {
    if (!isActive()) return;
    auto const coordinatorStatus = mCoordinator.status();
    if (mPhase == Phase::Finalizing || mPhase == Phase::Cancelling) {
        if (coordinatorStatus.state == ExportState::Completed) {
            restoreReplayState();
            setExportActivityActive(false);
            mPhase = Phase::Completed;
        } else if (coordinatorStatus.state == ExportState::Cancelled) {
            restoreReplayState();
            setExportActivityActive(false);
            mPhase = Phase::Cancelled;
        } else if (coordinatorStatus.state == ExportState::Faulted) {
            restoreReplayState();
            setExportActivityActive(false);
            mPhase = Phase::Faulted;
        }
        return;
    }
    if (!mPlan || !mRenderBoundary) {
        fail(ExportError::CaptureUnavailable, "The offline render boundary is no longer available");
        return;
    }
    if (coordinatorStatus.state == ExportState::Faulted) {
        closeCapture(true);
        restoreReplayState();
        setExportActivityActive(false);
        mPhase = Phase::Faulted;
        return;
    }
    if (coordinatorStatus.state == ExportState::Cancelled) {
        closeCapture(true);
        restoreReplayState();
        setExportActivityActive(false);
        mPhase = Phase::Cancelled;
        return;
    }

    auto const boundaryStatus = mRenderBoundary->status();
    if (boundaryStatus.state == OfflineRenderBoundaryState::Faulted) {
        fail(
            mapBoundaryError(boundaryStatus.error),
            boundaryStatus.message.empty() ? "The offline renderer failed" : boundaryStatus.message
        );
        return;
    }

    auto const submission = collectDownloads();
    if (submission == SubmissionResult::Failed || submission == SubmissionResult::Backpressured) return;

    if (mPhase == Phase::Draining) {
        if (mReadyFrames.empty() && mRenderBoundary->isDrained()) finish();
        return;
    }

    while (mPhase == Phase::Rendering) {
        auto frame = mPlan->frame(mNextFrameIndex);
        if (!frame) {
            fail(ExportError::InvalidTimeline, "The export frame plan ended unexpectedly");
            return;
        }

        switch (mRenderBoundary->advance(*frame)) {
        case OfflineRenderStepResult::Waiting:
        case OfflineRenderStepResult::Backpressured:
            return;
        case OfflineRenderStepResult::Failed: {
            auto const status = mRenderBoundary->status();
            fail(
                mapBoundaryError(status.error),
                status.message.empty() ? "The offline renderer failed" : status.message
            );
            return;
        }
        case OfflineRenderStepResult::FrameSubmitted:
            ++mNextFrameIndex;
            if (mNextFrameIndex >= mPlan->frameCount) {
                if (!mRenderBoundary->beginDrain()) {
                    fail(ExportError::CaptureFailed, "The offline renderer could not enter its drain phase");
                    return;
                }
                mPhase = Phase::Draining;
            }
            break;
        }
    }
}

void ReplayExportDriver::cancel() {
    if (!isActive() && mPhase != Phase::Faulted) return;
    if (mPhase == Phase::Cancelling) return;
    bool const preserveFailure = mPhase == Phase::Faulted || mCoordinator.status().state == ExportState::Faulted;
    getLogger().info(
        "Video export cancellation requested (phase {}, next frame {}, {} buffered frames)",
        static_cast<int>(mPhase),
        mNextFrameIndex,
        mReadyFrames.size()
    );
    closeCapture(true);
    mCoordinator.cancel();
    restoreReplayState();
    if (!preserveFailure) {
        mPhase = Phase::Cancelling;
    } else {
        setExportActivityActive(false);
    }
}

void ReplayExportDriver::reset() {
    if (isActive() || mPhase == Phase::Faulted) cancel();
    closeCapture(true);
    mCoordinator.reset();
    setExportActivityActive(false);
    mPlan.reset();
    mReadyFrames.clear();
    mNextFrameIndex = 0;
    mPhase          = Phase::Idle;
}

bool ReplayExportDriver::isAvailable() const {
    return mRenderBoundary != nullptr && screen::isIdleDetectionGuardInstalled() && isOfflineRenderClockInstalled();
}

bool ReplayExportDriver::isActive() const {
    return mPhase == Phase::Rendering || mPhase == Phase::Draining || mPhase == Phase::Finalizing
        || mPhase == Phase::Cancelling;
}

ReplayExportDriver::SubmissionResult ReplayExportDriver::submitReadyFrames() {
    while (!mReadyFrames.empty()) {
        auto const result = mCoordinator.trySubmit(mReadyFrames.front());
        if (result == FrameWriterSubmitResult::Backpressured) return SubmissionResult::Backpressured;
        if (result != FrameWriterSubmitResult::Accepted) {
            fail(ExportError::WriteFailed, "The export frame writer rejected a captured frame");
            return SubmissionResult::Failed;
        }
        mReadyFrames.pop_front();
    }
    return SubmissionResult::Ready;
}

ReplayExportDriver::SubmissionResult ReplayExportDriver::collectDownloads() {
    auto submission = submitReadyFrames();
    if (submission != SubmissionResult::Ready) return submission;

    while (mReadyFrames.size() < ExportCaptureCapacity) {
        auto frame = mRenderBoundary->finishDownload();
        if (!frame) break;
        mReadyFrames.emplace_back(std::move(*frame));
    }
    return submitReadyFrames();
}

ExportError ReplayExportDriver::mapBoundaryError(OfflineRenderBoundaryError error) const {
    switch (error) {
    case OfflineRenderBoundaryError::ReplayUnavailable:
    case OfflineRenderBoundaryError::ReplayFailed:
        return ExportError::ReplayUnavailable;
    case OfflineRenderBoundaryError::TickUnavailable:
    case OfflineRenderBoundaryError::ClockUnavailable:
    case OfflineRenderBoundaryError::CaptureUnavailable:
        return ExportError::CaptureUnavailable;
    case OfflineRenderBoundaryError::InvalidFrame:
        return ExportError::InvalidFrame;
    case OfflineRenderBoundaryError::None:
    case OfflineRenderBoundaryError::CaptureFailed:
    case OfflineRenderBoundaryError::InvalidState:
        return ExportError::CaptureFailed;
    }
    return ExportError::CaptureFailed;
}

void ReplayExportDriver::finish() {
    getLogger().info("Captured {} video export frames; finalizing output", mNextFrameIndex);
    closeCapture(false);
    if (!mCoordinator.finish()) {
        auto const status = mCoordinator.status();
        if (status.state == ExportState::Cancelling) {
            restoreReplayState();
            mPhase = Phase::Cancelling;
        } else {
            restoreReplayState();
            setExportActivityActive(false);
            mPhase = Phase::Faulted;
        }
        return;
    }
    restoreReplayState();
    mPhase = Phase::Finalizing;
}

void ReplayExportDriver::fail(ExportError error, std::string message) {
    getLogger().error(
        "Video export failed (phase {}, next frame {}, error {}): {}",
        static_cast<int>(mPhase),
        mNextFrameIndex,
        static_cast<int>(error),
        message
    );
    closeCapture(true);
    mCoordinator.fail(error, std::move(message));
    restoreReplayState();
    mPhase = Phase::Cancelling;
}

void ReplayExportDriver::restoreReplayState() {
    if (!mRestorePaused) return;
    mRestorePaused = false;
    if (mReplay.isActive()) (void)mReplay.setPaused(mPreviousPaused);
}

void ReplayExportDriver::closeCapture(bool cancelled) {
    setOfflineRenderActivityActive(false);
    if (mRenderBoundary) {
        if (cancelled) mRenderBoundary->cancel();
        else mRenderBoundary->close();
    }
    mReadyFrames.clear();
}

} // namespace playback::editor::exporting
