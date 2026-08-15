#pragma once

#include "ExportTypes.h"
#include "OfflineRenderClockHooks.h"
#include "OfflineRenderFrameExecutor.h"
#include "SaveableFramebufferQueue.h"

#include "playback/runtime/ClientTickHooks.h"

#include <chrono>
#include <cstddef>
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
    FrameDownloadQueueStatus         downloads;
    OfflineRenderFrameExecutorStatus executor;
    uint32_t                         warmupFramesRemaining{};
    uint32_t                         warmupStableFrames{};
};

class OfflineRenderBoundary {
public:
    OfflineRenderBoundary(replay::ReplaySession& replay, visuals::FrameTap& frameTap);
    ~OfflineRenderBoundary();

    OfflineRenderBoundary(OfflineRenderBoundary const&)            = delete;
    OfflineRenderBoundary& operator=(OfflineRenderBoundary const&) = delete;

    [[nodiscard]] bool
    open(
        uint32_t                              capacity,
        ExportSettings const&                 settings,
        state::editing::model::EditorStateExt const& project,
        std::optional<std::string>            cameraFallback = std::nullopt
    );
    void close();
    void cancel();

    [[nodiscard]] OfflineRenderStepResult advance(ExportFramePlan const& frame);
    [[nodiscard]] bool                    beginDrain();
    [[nodiscard]] bool                    isDrained();
    [[nodiscard]] bool retryCompletedFrame(visuals::FrameTicket const& ticket);

    [[nodiscard]] std::optional<visuals::CapturedFrame> finishDownload();

    [[nodiscard]] OfflineRenderBoundaryStatus status();

private:
    [[nodiscard]] int                                     targetTick(ExportFramePlan const& frame) const;
    [[nodiscard]] std::optional<OfflineRenderClockSample> clockSample(ExportFramePlan const& frame) const;
    [[nodiscard]] OfflineRenderStepResult                 advanceWarmup(ExportFramePlan const& frame);
    [[nodiscard]] bool                                    warmupComplete() const;
    [[nodiscard]] bool recoverDownloadFailure(FrameDownloadQueueStatus const& status);
    [[nodiscard]] bool publishClockSample(ExportFramePlan const& frame);
    void               clearClockSample();
    void               fault(OfflineRenderBoundaryError error, std::string message);

    replay::ReplaySession&                        mReplay;
    SaveableFramebufferQueue                         mDownloads;
    OfflineRenderFrameExecutor                       mExecutor;
    std::optional<ExportFramePlan>                   mPendingFrame;
    std::optional<ExportFramePlan>                   mLastSubmittedFrame;
    std::optional<visuals::FrameTicket>    mCompletedFrameTicket;
    std::optional<runtime::OfflineReplayTickToken> mReplayTickToken;
    std::optional<OfflineRenderClockToken>           mClockToken;
    int64_t                                          mMaximumReplayTick{};
    uint32_t                                         mCaptureCapacity{};
    uint32_t                                         mCaptureRetryCount{};
    uint32_t                                         mReplayTickRecoveryCount{};
    uint32_t                                         mWarmupFramesRemaining{};
    uint32_t                                         mWarmupStableFrames{};
    uint64_t                                         mRenderWaitPolls{};
    std::chrono::steady_clock::time_point            mRenderWaitStartedAt{};
    std::chrono::steady_clock::time_point            mRenderWaitLastLoggedAt{};
    std::chrono::steady_clock::time_point            mReplayTickRequestedAt{};
    std::chrono::steady_clock::time_point            mWarmupStartedAt{};
    std::chrono::steady_clock::time_point            mWarmupLastLoggedAt{};
    bool                                             mTickGateOpen{};
    bool                                             mTickGateSuspendedForDimension{};
    bool                                             mTimelineInitialized{};
    bool                                             mInitializationTickObserved{};
    OfflineRenderBoundaryState                       mState{OfflineRenderBoundaryState::Closed};
    OfflineRenderBoundaryError                       mError{OfflineRenderBoundaryError::None};
    std::string                                      mMessage;
};

} // namespace playback::exporting
