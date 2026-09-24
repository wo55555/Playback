// Must precede the Minecraft headers: they pull in a conflicting d3d12 declaration set.
#include "playback/editor/graphics/D3D12Hooks.h"

#include "ReplayExportDriver.h"

#include "ExportActivity.h"

#include "playback/Playback.h"
#include "playback/editor/graphics/CameraRenderHooks.h"
#include "playback/exporting/IdleDetectionHooks.h"
#include "playback/exporting/OfflineRenderTrace.h"
#include "playback/exporting/RenderDiagnostics.h"
#include "playback/replay/ReplaySession.h"
#include "playback/state/editing/models/EditorStateExt.h"

#include <filesystem>
#include <utility>

namespace playback::exporting {

namespace {

// Deeper than the in-flight capture count so a full writer queue does not stall the next arm.
constexpr uint32_t ExportCaptureCapacity = 6;

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

std::filesystem::path tracePathFor(CompiledExportPlan const& plan) {
    return plan.outputPath.parent_path() / (plan.outputPath.filename().string() + ".render-trace.csv");
}

} // namespace

ReplayExportDriver::ReplayExportDriver(ExportCoordinator& coordinator, replay::ReplaySession& replay)
: mCoordinator(coordinator),
  mReplay(replay) {}

ReplayExportDriver::~ReplayExportDriver() { reset(); }

void ReplayExportDriver::setRendererAvailable(bool available) {
    if (mRenderBoundary && isActive()) cancel();
    mRenderBoundary.reset();
    if (available) mRenderBoundary = std::make_unique<OfflineRenderBoundary>(mReplay);
}

bool ReplayExportDriver::start(
    ExportSettings                               settings,
    state::editing::model::EditorStateExt const& project,
    std::optional<std::string>                   cameraFallback
) {
    if (isActive()) return false;
    if (!exporting::isIdleDetectionGuardInstalled()) {
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
    if (!mReplay.isReadyForExport()) {
        (void)hookOfflineRenderClock(false);
        mCoordinator.fail(
            ExportError::ReplayUnavailable,
            "The replay is still loading; wait for the replay scene to become ready before starting export"
        );
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

    bool const previousPaused = mReplay.isPaused();
    if (!mReplay.setPaused(true)) {
        (void)hookOfflineRenderClock(false);
        fail(ExportError::ReplayUnavailable, "Unable to pause the replay for export");
        return false;
    }
    mPreviousPaused = previousPaused;

    if (renderDiagnosticsEnabled()) {
        auto const tracePath = tracePathFor(*mPlan);
        if (beginOfflineRenderTrace(tracePath)) {
            recordOfflineRenderTrace(
                OfflineRenderTraceEvent::SessionBegin,
                nullptr,
                nullptr,
                mPlan->frameCount,
                mPlan->settings.resolutionX,
                mPlan->settings.resolutionY,
                mPlan->settings.ssaa
            );
            auto const hooks = editor::graphics::cameraRenderHookInventory();
            recordOfflineRenderTrace(
                OfflineRenderTraceEvent::HookInventory,
                nullptr,
                nullptr,
                hooks.renderFrame ? 1 : 0,
                hooks.upscaling ? 1 : 0,
                mPlan->settings.warmupFrames,
                offlineRenderClockDiagnosticHookMask()
            );
            getLogger().info("[RenderDiag] export render trace={}", tracePath);
        } else {
            getLogger().warn("Export render trace could not be opened: {}", tracePath);
        }
    }
    recordRenderDiagnostics(RenderDiagnosticStage::ExportBeforeOpen);
    bool const opened =
        mRenderBoundary->open(ExportCaptureCapacity, mPlan->settings, project, std::move(cameraFallback));
    recordRenderDiagnostics(RenderDiagnosticStage::ExportAfterOpen);
    if (!opened) {
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
    mNextFrameIndex = 0;
    mWaitStartedAt  = {};
    mWaitReason.reset();
    mWaitMicros.fill(0);
    mWaitHits.fill(0);
    mWaitChargedAt = {};
    mWaitCharged.reset();
    mDriverTicks       = 0;
    auto const profile = renderDiagnosticProfile();
    if (profile.enabled) {
        getLogger().info(
            "[RenderDiag] profile configured={}, known={}, probe={}, actualMask={}, expectedMask={}, traceOpen={}, "
            "submitRequestedMask={}, submitActualMask={}",
            Playback::getInstance().getConfig().renderDiagnosticExperiment,
            profile.known,
            static_cast<int>(profile.probe),
            offlineRenderClockDiagnosticHookMask(),
            profile.hookMask(),
            isOfflineRenderTraceActive(),
            profile.submitHookMask(),
            editor::graphics::offlineSubmitHookMask()
        );
        if (!profile.known) getLogger().warn("Unknown render diagnostic experiment; using observation only");
    }
    setExportActivityActive(true);
    mPhase = Phase::Rendering;
    getLogger().info(
        "Video export started: output={}, frames={}, ticks={}-{}, fps={}/{}, resolution={}x{}, ssaa={}, "
        "warmup={}, convergence={}",
        mPlan->outputPath,
        mPlan->frameCount,
        mPlan->settings.startTick,
        mPlan->settings.endTick,
        mPlan->settings.frameRate.numerator,
        mPlan->settings.frameRate.denominator,
        mPlan->settings.resolutionX,
        mPlan->settings.resolutionY,
        mPlan->settings.ssaa,
        mPlan->settings.warmupFrames,
        mPlan->settings.convergenceFrames
    );
    return true;
}

void ReplayExportDriver::tick() {
    if (!isActive()) return;
    ++mDriverTicks;
    OfflineRenderTraceScope tickTrace(
        OfflineRenderTraceEvent::DriverTickEnter,
        OfflineRenderTraceEvent::DriverTickExit,
        this,
        nullptr,
        0,
        mNextFrameIndex,
        mReadyFrames.size(),
        static_cast<uint64_t>(mPhase)
    );
    auto const coordinatorStatus = mCoordinator.status();
    recordOfflineRenderTrace(
        OfflineRenderTraceEvent::DriverStage,
        this,
        nullptr,
        1,
        mNextFrameIndex,
        coordinatorStatus.submittedFrames,
        coordinatorStatus.writtenFrames
    );
    if (mPhase == Phase::Finalizing || mPhase == Phase::Cancelling) {
        if (coordinatorStatus.state == ExportState::Completed) {
            restoreReplayState();
            setExportActivityActive(false);
            mPhase = Phase::Completed;
            getLogger().info("Video export completed: {} frames written to {}", mNextFrameIndex, mPlan->outputPath);
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
        recordWait(OfflineRenderWaitReason::Failed);
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
    recordOfflineRenderTrace(
        OfflineRenderTraceEvent::DriverStage,
        this,
        nullptr,
        2,
        mNextFrameIndex,
        static_cast<uint64_t>(boundaryStatus.state),
        mReadyFrames.size()
    );
    if (boundaryStatus.state == OfflineRenderBoundaryState::Faulted) {
        recordWait(OfflineRenderWaitReason::Failed);
        fail(
            mapBoundaryError(boundaryStatus.error),
            boundaryStatus.message.empty() ? "The offline renderer failed" : boundaryStatus.message
        );
        return;
    }

    auto const submission = collectDownloads();
    if (submission == SubmissionResult::Failed || submission == SubmissionResult::Backpressured) {
        waitFor(
            tickTrace,
            submission == SubmissionResult::Backpressured ? OfflineRenderWaitReason::WriterBackpressure
                                                          : OfflineRenderWaitReason::Failed,
            submission == SubmissionResult::Backpressured ? 1 : 4
        );
        return;
    }

    if (mPhase == Phase::Draining) {
        waitFor(tickTrace, OfflineRenderWaitReason::Draining, 5);
        if (mReadyFrames.empty() && mRenderBoundary->isDrained()) finish();
        return;
    }

    while (mPhase == Phase::Rendering) {
        auto frame = mPlan->frame(mNextFrameIndex);
        if (!frame) {
            fail(ExportError::InvalidTimeline, "The export frame plan ended unexpectedly");
            return;
        }

        auto const step = mRenderBoundary->advance(*frame);
        switch (step) {
        case OfflineRenderStepResult::Waiting:
            waitFor(tickTrace, mRenderBoundary->lastWaitReason(), 2);
            return;
        case OfflineRenderStepResult::Backpressured:
            waitFor(tickTrace, mRenderBoundary->lastWaitReason(), 3);
            return;
        case OfflineRenderStepResult::Failed: {
            recordWait(OfflineRenderWaitReason::Failed);
            tickTrace.result(4);
            auto const status = mRenderBoundary->status();
            fail(
                mapBoundaryError(status.error),
                status.message.empty() ? "The offline renderer failed" : status.message
            );
            return;
        }
        case OfflineRenderStepResult::FrameSubmitted:
            recordWait(OfflineRenderWaitReason::None);
            accumulateWait(std::chrono::steady_clock::now());
            mWaitCharged = OfflineRenderWaitReason::None;
            ++mWaitHits[static_cast<size_t>(OfflineRenderWaitReason::None)];
            mWaitStartedAt = {};
            mWaitReason.reset();
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
    getLogger().info("Video export cancelled after {} frames", mNextFrameIndex);
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
    mWaitStartedAt  = {};
    mWaitReason.reset();
    mPhase = Phase::Idle;
}

bool ReplayExportDriver::isAvailable() const {
    return mRenderBoundary != nullptr && exporting::isIdleDetectionGuardInstalled();
}

bool ReplayExportDriver::isActive() const {
    return mPhase == Phase::Rendering || mPhase == Phase::Draining || mPhase == Phase::Finalizing
        || mPhase == Phase::Cancelling;
}

ReplayExportDriver::SubmissionResult ReplayExportDriver::submitReadyFrames() {
    while (!mReadyFrames.empty()) {
        auto const result = [&] {
            OfflineRenderTraceScope submitTrace(
                OfflineRenderTraceEvent::WriterSubmitEnter,
                OfflineRenderTraceEvent::WriterSubmitExit,
                this,
                &mCoordinator,
                0,
                mReadyFrames.front().ticket.frameIndex,
                mReadyFrames.size()
            );
            submitTrace.result(UINT64_MAX);
            auto const submitted = mCoordinator.trySubmit(mReadyFrames.front());
            submitTrace.result(static_cast<uint64_t>(submitted));
            return submitted;
        }();
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
    auto const submission = submitReadyFrames();
    if (submission == SubmissionResult::Failed) {
        return submission;
    }

    // Draining even while backpressured is what keeps the boundary from re-rendering its armed sample forever.
    while (mReadyFrames.size() < ExportCaptureCapacity) {
        auto frame = mRenderBoundary->finishDownload();
        if (!frame) break;
        mReadyFrames.emplace_back(std::move(*frame));
    }
    auto const result = submitReadyFrames();
    return result;
}

void ReplayExportDriver::waitFor(OfflineRenderTraceScope& trace, OfflineRenderWaitReason reason, uint64_t result) {
    auto const now = std::chrono::steady_clock::now();
    accumulateWait(now);
    mWaitCharged = reason;
    ++mWaitHits[static_cast<size_t>(reason)];
    // Holds from the first waiting frame, so the renderer keeps drawing for the whole wait.
    if (!mWaitReason || *mWaitReason != reason) {
        mWaitReason    = reason;
        mWaitStartedAt = now;
    }
    if (mRenderBoundary) {
        mRenderBoundary->holdRenderAlive(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - mWaitStartedAt).count())
        );
    }
    recordWait(reason);
    trace.result(result);
}

void ReplayExportDriver::reportWaitProfile() const {
    if (mDriverTicks == 0) return;
    static constexpr std::array<char const*, WaitReasonCount> names{
        "None",
        "WriterBackpressure",
        "CaptureCapacity",
        "ReplayPreparation",
        "DimensionTransition",
        "UiStable",
        "NativeTick",
        "WarmupCpu",
        "WarmupBudget",
        "WarmupUi",
        "CaptureArm",
        "CpuSample",
        "CapturePending",
        "CollectPending",
        "Draining",
        "Failed",
        "Convergence",
        "Unknown",
    };
    uint64_t total = 0;
    for (auto const micros : mWaitMicros) total += micros;
    auto const frames = std::max<uint64_t>(mNextFrameIndex, 1);

    std::string breakdown;
    for (size_t index = 0; index < WaitReasonCount; ++index) {
        if (mWaitMicros[index] == 0 && mWaitHits[index] == 0) continue;
        if (!breakdown.empty()) breakdown += ", ";
        breakdown += fmt::format(
            "{}={:.1f}ms ({:.2f}/frame, {} hits)",
            names[index],
            static_cast<double>(mWaitMicros[index]) / 1000.0,
            static_cast<double>(mWaitMicros[index]) / 1000.0 / static_cast<double>(frames),
            mWaitHits[index]
        );
    }
    getLogger().debug(
        "Export wait profile: frames={}, driverTicks={} ({:.2f}/frame), totalMs={:.0f} ({:.2f}/frame); {}",
        mNextFrameIndex,
        mDriverTicks,
        static_cast<double>(mDriverTicks) / static_cast<double>(frames),
        static_cast<double>(total) / 1000.0,
        static_cast<double>(total) / 1000.0 / static_cast<double>(frames),
        breakdown
    );
}

void ReplayExportDriver::accumulateWait(std::chrono::steady_clock::time_point now) {
    if (mWaitCharged && mWaitChargedAt != std::chrono::steady_clock::time_point{}) {
        mWaitMicros[static_cast<size_t>(*mWaitCharged)] +=
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - mWaitChargedAt).count());
    }
    mWaitChargedAt = now;
}

void ReplayExportDriver::recordWait(OfflineRenderWaitReason reason) const noexcept {
    recordOfflineRenderTrace(
        OfflineRenderTraceEvent::DriverWait,
        this,
        nullptr,
        static_cast<uint64_t>(reason),
        mNextFrameIndex,
        mReadyFrames.empty() ? UINT64_MAX : mReadyFrames.front().ticket.frameIndex,
        static_cast<uint64_t>(mPhase)
    );
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
    getLogger().error("Video export failed at frame {}: {}", mNextFrameIndex, message);
    closeCapture(true);
    mCoordinator.fail(error, std::move(message));
    restoreReplayState();
    mPhase = Phase::Cancelling;
}

void ReplayExportDriver::restoreReplayState() {
    if (!mPreviousPaused) return;
    bool const previousPaused = *mPreviousPaused;
    mPreviousPaused.reset();
    if (mReplay.isActive()) (void)mReplay.setPaused(previousPaused);
}

void ReplayExportDriver::closeCapture(bool cancelled) {
    reportWaitProfile();
    setOfflineRenderActivityActive(false);
    if (mRenderBoundary) {
        if (cancelled) mRenderBoundary->cancel();
        else mRenderBoundary->close();
    }
    if (!hookOfflineRenderClock(false)) {
        getLogger().error("Unable to remove export-scoped offline render hooks after capture close");
    }
    mReadyFrames.clear();
    if (isOfflineRenderTraceActive()) {
        recordRenderDiagnostics(RenderDiagnosticStage::ExportClosed);
        recordOfflineRenderTrace(OfflineRenderTraceEvent::SessionEnd, nullptr, nullptr, cancelled ? 1 : 0);
        if (!finishOfflineRenderTrace()) getLogger().error("Unable to flush the export render trace");
    }
}

} // namespace playback::exporting
