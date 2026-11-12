#pragma once

#include "ExportTypes.h"
#include "OfflineRenderClockHooks.h"
#include "OfflineRenderFrameExecutor.h"
#include "OfflineRenderWaitReason.h"

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
    [[nodiscard]] bool                                    hasStep(OfflineRenderClockKind kind) const;
    // A step that is still published is a fault.
    [[nodiscard]] bool publishStep(ExportFramePlan const& frame, OfflineRenderClockKind kind);
    // Renders the frame's scene without a capture a fixed number of times; nothing measures whether it has settled.
    [[nodiscard]] bool advanceExtraRenders(ExportFramePlan const& frame);
    // How long finishDownload may block for the capture of the render that has just returned; zero is a plain poll.
    [[nodiscard]] std::chrono::milliseconds submittedCaptureWait();
    void               clearStep();
    void               fault(OfflineRenderBoundaryError error, std::string message);

    // The clock hooks hold one render step at a time.
    struct PublishedStep {
        OfflineRenderClockToken token;
        OfflineRenderClockKind  kind{OfflineRenderClockKind::KeepAlive};
    };

    replay::ReplaySession&                         mReplay;
    uint32_t                                       mCaptureCapacity{};
    bool                                           mCaptureArmed{};
    // Each render of the capture step is blocked on once; a capture that misses that wait is polled for.
    bool                                           mSubmittedCaptureAwaited{};
    OfflineRenderFrameExecutor                     mExecutor;
    std::optional<ExportFramePlan>                 mPendingFrame;
    std::optional<ExportFramePlan>                 mLastSubmittedFrame;
    std::optional<visuals::FrameTicket>            mCompletedFrameTicket;
    std::optional<runtime::OfflineReplayTickToken> mReplayTickToken;
    std::optional<PublishedStep>                   mStep;
    // Set once a step of the pending frame carried the time since the last submitted frame.
    bool                                           mFrameTimeSpent{};
    int64_t                                        mMaximumReplayTick{};
    // The configured warm-up budget; it is owed again after every dimension change.
    uint32_t                                       mWarmupFrames{};
    uint32_t                                       mWarmupFramesRemaining{};
    uint32_t                                       mWarmupStableFrames{};
    uint32_t                                       mExtraRendersDone{};
    bool                                           mExtraRendersComplete{};
    // Per-frame wall clock split three ways, because tick gating before the first extra render poll is neither.
    std::chrono::steady_clock::time_point mFrameStartedAt{};
    std::chrono::steady_clock::time_point mExtraRendersStartedAt{};
    std::chrono::steady_clock::time_point mExtraRendersEndedAt{};
    uint64_t                              mPrepareMicros{};
    uint64_t                              mExtraRenderMicros{};
    uint64_t                              mCaptureMicros{};
    uint64_t                              mExtraRenderPasses{};
    uint64_t                              mCaptureWaits{};
    uint64_t                              mCaptureWaitsMissed{};
    uint64_t                              mProfiledFrames{};
    std::chrono::steady_clock::time_point mRenderWaitStartedAt{};
    std::chrono::steady_clock::time_point mRenderWaitLastLoggedAt{};
    std::chrono::steady_clock::time_point mReplayTickRequestedAt{};
    std::chrono::steady_clock::time_point mWarmupStartedAt{};
    std::chrono::steady_clock::time_point mWarmupLastLoggedAt{};
    bool                                  mTickGateOpen{};
    bool                                  mTickGateSuspendedForDimension{};
    bool                                  mDimensionLoadingForced{};
    bool                                  mTimelineInitialized{};
    bool                                  mInitializationTickObserved{};
    OfflineRenderBoundaryState            mState{OfflineRenderBoundaryState::Closed};
    OfflineRenderBoundaryError            mError{OfflineRenderBoundaryError::None};
    OfflineRenderWaitReason               mLastWaitReason{OfflineRenderWaitReason::Unknown};
    std::string                           mMessage;
};

} // namespace playback::exporting
