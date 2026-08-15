#include "ReplayExportDriver.h"

#include "ExportActivity.h"
#include "FrameWriterUtils.h"

#include "playback/Playback.h"
#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/replay/ReplaySession.h"
#include "playback/screen/IdleDetectionHooks.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace playback::editor::exporting {

namespace {

constexpr uint32_t ExportCaptureCapacity  = 4;
constexpr uint32_t MaxClearFrameRerenders = 24;
constexpr uint8_t  ExportClearRed         = 10;
constexpr uint8_t  ExportClearGreen       = 12;
constexpr uint8_t  ExportClearBlue        = 22;

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

struct UniformFrameProbe {
    uint8_t minimum[3]{255, 255, 255};
    uint8_t maximum[3]{};
};

UniformFrameProbe probeUniformFrame(visuals::CapturedFrame const& frame) {
    UniformFrameProbe probe;
    if (!detail::validateFrame(frame)) return probe;

    auto const*    pixels = reinterpret_cast<uint8_t const*>(frame.pixels.data());
    uint32_t const stepX  = std::max<uint32_t>(1, frame.width / 64);
    uint32_t const stepY  = std::max<uint32_t>(1, frame.height / 36);
    for (uint32_t y = 0; y < frame.height; y += stepY) {
        auto const* row = pixels + static_cast<size_t>(y) * frame.rowPitch;
        for (uint32_t x = 0; x < frame.width; x += stepX) {
            auto const* pixel = row + static_cast<size_t>(x) * 4;
            for (size_t channel = 0; channel < 3; ++channel) {
                probe.minimum[channel] = std::min(probe.minimum[channel], pixel[channel]);
                probe.maximum[channel] = std::max(probe.maximum[channel], pixel[channel]);
            }
        }
    }
    return probe;
}

bool isKnownExportClearFrame(visuals::CapturedFrame const& frame, UniformFrameProbe const& probe) {
    if (probe.minimum[0] != ExportClearRed || probe.maximum[0] != ExportClearRed || probe.minimum[1] != ExportClearGreen
        || probe.maximum[1] != ExportClearGreen || probe.minimum[2] != ExportClearBlue
        || probe.maximum[2] != ExportClearBlue) {
        return false;
    }

    auto const* pixels = reinterpret_cast<uint8_t const*>(frame.pixels.data());
    for (uint32_t y = 0; y < frame.height; ++y) {
        auto const* row = pixels + static_cast<size_t>(y) * frame.rowPitch;
        for (uint32_t x = 0; x < frame.width; ++x) {
            auto const* pixel = row + static_cast<size_t>(x) * 4;
            if (pixel[0] != ExportClearRed || pixel[1] != ExportClearGreen || pixel[2] != ExportClearBlue) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

ReplayExportDriver::ReplayExportDriver(ExportCoordinator& coordinator, replay::ReplaySession& replay)
: mCoordinator(coordinator),
  mReplay(replay) {}

ReplayExportDriver::~ReplayExportDriver() { reset(); }

void ReplayExportDriver::setFrameTap(visuals::FrameTap* frameTap) {
    if (mRenderBoundary && isActive()) cancel();
    mRenderBoundary.reset();
    if (frameTap) mRenderBoundary = std::make_unique<OfflineRenderBoundary>(mReplay, *frameTap);
}

bool ReplayExportDriver::start(
    ExportSettings                        settings,
    editing::model::EditorStateExt const& project,
    std::optional<std::string>            cameraFallback
) {
    if (isActive()) return false;
    if (!screen::isIdleDetectionGuardInstalled()) {
        mCoordinator.fail(
            ExportError::CaptureUnavailable,
            "Minecraft's idle detection guard is unavailable; export was blocked to prevent invalid frames"
        );
        mPhase = Phase::Faulted;
        return false;
    }
    if (!isOfflineRenderClockInstalled() && !hookOfflineRenderClock(true)) {
        mCoordinator.fail(
            ExportError::CaptureUnavailable,
            "The offline render clock could not be installed for this export"
        );
        mPhase = Phase::Faulted;
        return false;
    }
    if (!mRenderBoundary) {
        (void)hookOfflineRenderClock(false);
        mCoordinator.fail(ExportError::CaptureUnavailable, "The offline renderer boundary is unavailable");
        mPhase = Phase::Faulted;
        return false;
    }
    if (!mReplay.isActive()) {
        (void)hookOfflineRenderClock(false);
        mCoordinator.fail(ExportError::ReplayUnavailable, "A replay must be active before video export can start");
        mPhase = Phase::Faulted;
        return false;
    }

    if (!mCoordinator.start(std::move(settings), project)) {
        (void)hookOfflineRenderClock(false);
        mPhase = Phase::Faulted;
        return false;
    }

    mPlan = mCoordinator.plan();
    if (!mPlan) {
        (void)hookOfflineRenderClock(false);
        fail(ExportError::InvalidSettings, "The export plan was not retained by the coordinator");
        return false;
    }

    mPreviousPaused = mReplay.isPaused();
    if (!mReplay.setPaused(true)) {
        (void)hookOfflineRenderClock(false);
        fail(ExportError::ReplayUnavailable, "Unable to pause the replay for export");
        return false;
    }
    mRestorePaused = true;

    if (!mRenderBoundary->open(ExportCaptureCapacity, mPlan->settings, project, std::move(cameraFallback))) {
        (void)hookOfflineRenderClock(false);
        auto const boundaryStatus = mRenderBoundary->status();
        fail(
            ExportError::CaptureUnavailable,
            boundaryStatus.executor.message.empty() ? "The offline renderer could not be opened"
                                                    : boundaryStatus.executor.message
        );
        return false;
    }
    mReadyFrames.clear();
    mClearFrameRetryCount    = 0;
    mRejectedClearFrameCount = 0;
    mNextFrameIndex          = 0;
    setExportActivityActive(true);
    mPhase = Phase::Rendering;
    getLogger().info(
        "Video export started: output={}, format={}, frames={}, ticks={}-{}, fps={}/{}, resolution={}x{}, ssaa={}, "
        "warmup={}, replayTick={}",
        mPlan->outputPath,
        static_cast<int>(mPlan->settings.format),
        mPlan->frameCount,
        mPlan->settings.startTick,
        mPlan->settings.endTick,
        mPlan->settings.frameRate.numerator,
        mPlan->settings.frameRate.denominator,
        mPlan->settings.resolutionX,
        mPlan->settings.resolutionY,
        mPlan->settings.ssaa,
        mPlan->settings.warmupFrames,
        mReplay.getAppliedReplayTick()
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
    mClearFrameRetryCount    = 0;
    mRejectedClearFrameCount = 0;
    mNextFrameIndex          = 0;
    mPhase                   = Phase::Idle;
}

bool ReplayExportDriver::isAvailable() const {
    return mRenderBoundary != nullptr && screen::isIdleDetectionGuardInstalled();
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
        if (!mPlan || !detail::normalizeFrame(*frame, mPlan->settings.resolutionX, mPlan->settings.resolutionY)) {
            fail(ExportError::InvalidFrame, "The captured frame could not be normalized to the export resolution");
            return SubmissionResult::Failed;
        }
        auto const probe = probeUniformFrame(*frame);
        if (isKnownExportClearFrame(*frame, probe)) {
            ++mRejectedClearFrameCount;
            if (mRejectedClearFrameCount <= 4 || frame->ticket.frameIndex % 120 == 0) {
                getLogger().warn(
                    "Rejected uniform engine clear export frame (frame={}, retry={}/{}, count={})",
                    frame->ticket.frameIndex,
                    mClearFrameRetryCount + 1,
                    MaxClearFrameRerenders,
                    mRejectedClearFrameCount
                );
            }
            if (mClearFrameRetryCount >= MaxClearFrameRerenders) {
                fail(
                    ExportError::CaptureFailed,
                    "The renderer produced only clear surfaces for the same export sample"
                );
                return SubmissionResult::Failed;
            }
            ++mClearFrameRetryCount;
            if (!mRenderBoundary->retryCompletedFrame(frame->ticket)) {
                fail(
                    ExportError::CaptureFailed,
                    "The clear export frame could not be scheduled for a same-sample retry"
                );
                return SubmissionResult::Failed;
            }
            continue;
        }
        mClearFrameRetryCount = 0;
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
    getLogger().info(
        "Captured {} video export frames; finalizing output (rejected clear surfaces={})",
        mNextFrameIndex,
        mRejectedClearFrameCount
    );
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
    if (isOfflineRenderClockInstalled() && !hookOfflineRenderClock(false)) {
        getLogger().error("Unable to remove export-scoped offline render hooks after capture close");
    }
    mReadyFrames.clear();
}

} // namespace playback::editor::exporting
