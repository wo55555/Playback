#pragma once

#include "ExportTypes.h"
#include "OfflineRenderClockHooks.h"
#include "OfflineRenderFrameExecutor.h"
#include "OfflineRenderTrace.h"

#include "playback/runtime/ClientTickHooks.h"
#include "playback/visuals/FrameTap.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace playback::replay {
class ReplaySession;
}

namespace playback::exporting {

enum class OfflineRenderBoundaryState : uint8_t {
    Closed,
    Ready,
    InitializingReplay,
    PreparingReplay,
    WarmingUp,
    AwaitingDownload,
    Draining,
    Cancelled,
    Faulted,
};

enum class OfflineRenderBoundaryError : uint8_t {
    None,
    ReplayUnavailable,
    ReplayFailed,
    TickUnavailable,
    ClockUnavailable,
    CaptureUnavailable,
    CaptureFailed,
    InvalidFrame,
    InvalidState,
};

enum class OfflineRenderStepResult : uint8_t { Waiting, Backpressured, FrameSubmitted, Failed };

struct OfflineRenderBoundaryStatus {
    OfflineRenderBoundaryState       state{OfflineRenderBoundaryState::Closed};
    OfflineRenderBoundaryError       error{OfflineRenderBoundaryError::None};
    std::string                      message;
    visuals::FrameTapStatus          capture;
    OfflineRenderFrameExecutorStatus executor;
    uint32_t                         warmupFramesRemaining{};
    uint32_t                         warmupStableFrames{};
};

class OfflineRenderBoundary {
public:
    explicit OfflineRenderBoundary(replay::ReplaySession& replay);
    ~OfflineRenderBoundary();

    OfflineRenderBoundary(OfflineRenderBoundary const&)            = delete;
    OfflineRenderBoundary& operator=(OfflineRenderBoundary const&) = delete;

    [[nodiscard]] bool open(
        uint32_t                                     capacity,
        ExportSettings const&                        settings,
        state::editing::model::EditorStateExt const& project,
        std::optional<std::string>                   cameraFallback = std::nullopt
    );
    void close();
    void cancel();

    [[nodiscard]] OfflineRenderStepResult advance(ExportFramePlan const& frame);
    [[nodiscard]] bool                    beginDrain();
    [[nodiscard]] bool                    isDrained();

    // Republishes the last submitted frame without a capture so the game keeps drawing through a driver stall.
    void holdRenderAlive(uint64_t waitedMicros);

    [[nodiscard]] std::optional<visuals::CapturedFrame> finishDownload();

    [[nodiscard]] OfflineRenderBoundaryStatus status();
    [[nodiscard]] OfflineRenderWaitReason     lastWaitReason() const noexcept { return mLastWaitReason; }

private:
    [[nodiscard]] OfflineRenderStepResult
    waiting(OfflineRenderWaitReason reason, OfflineRenderStepResult result = OfflineRenderStepResult::Waiting) noexcept;
    [[nodiscard]] int                                     targetTick(ExportFramePlan const& frame) const;
    [[nodiscard]] std::optional<OfflineRenderClockSample> clockSample(ExportFramePlan const& frame) const;
    void                                                  updateExportCamera(ExportFramePlan const& frame);
    [[nodiscard]] OfflineRenderStepResult                 advanceWarmup(ExportFramePlan const& frame);
    [[nodiscard]] bool                                    warmupComplete() const;
    [[nodiscard]] bool publishClockSample(ExportFramePlan const& frame, bool captureSample);
    // Renders the frame's own time without a capture until the ray-traced denoiser has settled on it.
    [[nodiscard]] bool advanceConvergence(ExportFramePlan const& frame);
    void               releaseHeldRender();
    void               clearClockSample();
    void               fault(OfflineRenderBoundaryError error, std::string message);

    replay::ReplaySession&                         mReplay;
    uint32_t                                       mCaptureCapacity{};
    uint64_t                                       mTraceEpoch{};
    bool                                           mCaptureArmed{};
    OfflineRenderFrameExecutor                     mExecutor;
    std::optional<ExportFramePlan>                 mPendingFrame;
    std::optional<ExportFramePlan>                 mLastSubmittedFrame;
    std::optional<visuals::FrameTicket>            mCompletedFrameTicket;
    std::optional<runtime::OfflineReplayTickToken> mReplayTickToken;
    std::optional<OfflineRenderClockToken>         mClockToken;
    std::optional<OfflineRenderClockToken>         mHoldToken;
    int64_t                                        mMaximumReplayTick{};
    uint32_t                                       mWarmupFramesRemaining{};
    uint32_t                                       mWarmupStableFrames{};
    uint32_t                                       mConvergenceRendersDone{};
    bool                                           mConvergenceComplete{};
    std::optional<OfflineRenderClockToken>         mConvergenceToken{};
    // Per-frame wall clock split three ways, because tick gating before the first convergence poll is neither.
    std::chrono::steady_clock::time_point mFrameStartedAt{};
    std::chrono::steady_clock::time_point mConvergenceStartedAt{};
    std::chrono::steady_clock::time_point mConvergenceEndedAt{};
    uint64_t                              mPrepareMicros{};
    uint64_t                              mConvergenceMicros{};
    uint64_t                              mCaptureMicros{};
    uint64_t                              mConvergencePasses{};
    uint64_t                              mProfiledFrames{};
    std::chrono::steady_clock::time_point mRenderWaitStartedAt{};
    std::chrono::steady_clock::time_point mRenderWaitLastLoggedAt{};
    std::chrono::steady_clock::time_point mReplayTickRequestedAt{};
    std::chrono::steady_clock::time_point mWarmupStartedAt{};
    std::chrono::steady_clock::time_point mWarmupLastLoggedAt{};
    bool                                  mTickGateOpen{};
    bool                                  mTickGateSuspendedForDimension{};
    bool                                  mTimelineInitialized{};
    bool                                  mInitializationTickObserved{};
    OfflineRenderBoundaryState            mState{OfflineRenderBoundaryState::Closed};
    OfflineRenderBoundaryError            mError{OfflineRenderBoundaryError::None};
    OfflineRenderWaitReason               mLastWaitReason{OfflineRenderWaitReason::Unknown};
    std::string                           mMessage;
};

} // namespace playback::exporting
