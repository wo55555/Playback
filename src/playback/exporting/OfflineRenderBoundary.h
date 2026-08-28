#pragma once

#include "ExportTypes.h"
#include "OfflineRenderClockHooks.h"
#include "OfflineRenderFrameExecutor.h"

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

private:
    [[nodiscard]] int                                     targetTick(ExportFramePlan const& frame) const;
    [[nodiscard]] std::optional<OfflineRenderClockSample> clockSample(ExportFramePlan const& frame) const;
    void                                                  updateExportCamera(ExportFramePlan const& frame);
    [[nodiscard]] OfflineRenderStepResult                 advanceWarmup(ExportFramePlan const& frame);
    [[nodiscard]] bool                                    warmupComplete() const;
    [[nodiscard]] bool                                    publishClockSample(ExportFramePlan const& frame);
    void                                                  clearClockSample();
    void                                                  fault(OfflineRenderBoundaryError error, std::string message);

    replay::ReplaySession&                         mReplay;
    uint32_t                                       mCaptureCapacity{};
    bool                                           mCaptureArmed{};
    OfflineRenderFrameExecutor                     mExecutor;
    std::optional<ExportFramePlan>                 mPendingFrame;
    std::optional<ExportFramePlan>                 mLastSubmittedFrame;
    std::optional<visuals::FrameTicket>            mCompletedFrameTicket;
    std::optional<runtime::OfflineReplayTickToken> mReplayTickToken;
    std::optional<OfflineRenderClockToken>         mClockToken;
    int64_t                                        mMaximumReplayTick{};
    uint32_t                                       mWarmupFramesRemaining{};
    uint32_t                                       mWarmupStableFrames{};
    std::chrono::steady_clock::time_point          mRenderWaitStartedAt{};
    std::chrono::steady_clock::time_point          mRenderWaitLastLoggedAt{};
    std::chrono::steady_clock::time_point          mReplayTickRequestedAt{};
    std::chrono::steady_clock::time_point          mWarmupStartedAt{};
    std::chrono::steady_clock::time_point          mWarmupLastLoggedAt{};
    bool                                           mTickGateOpen{};
    bool                                           mTickGateSuspendedForDimension{};
    bool                                           mTimelineInitialized{};
    bool                                           mInitializationTickObserved{};
    OfflineRenderBoundaryState                     mState{OfflineRenderBoundaryState::Closed};
    OfflineRenderBoundaryError                     mError{OfflineRenderBoundaryError::None};
    std::string                                    mMessage;
};

} // namespace playback::exporting
