#pragma once

#include "ExportTypes.h"
#include "OfflineRenderClockHooks.h"
#include "OfflineRenderFrameExecutor.h"
#include "SaveableFramebufferQueue.h"

#include "playback/functions/tick/ClientTickHooks.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace playback::functions {
class ReplaySession;
}

namespace playback::editor::exporting {

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
};

// Owns the boundary between replay-sample preparation and its rendered output.
// A sample cannot advance until the matching GPU download has completed.
class OfflineRenderBoundary {
public:
    OfflineRenderBoundary(functions::ReplaySession& replay, functions::render::FrameTap& frameTap);
    ~OfflineRenderBoundary();

    OfflineRenderBoundary(OfflineRenderBoundary const&)            = delete;
    OfflineRenderBoundary& operator=(OfflineRenderBoundary const&) = delete;

    [[nodiscard]] bool
         open(uint32_t capacity, ExportSettings const& settings, editing::model::EditorStateExt const& project);
    void close();
    void cancel();

    [[nodiscard]] OfflineRenderStepResult advance(ExportFramePlan const& frame);
    [[nodiscard]] bool                    beginDrain();
    [[nodiscard]] bool                    isDrained();

    [[nodiscard]] std::optional<functions::render::CapturedFrame> finishDownload();

    [[nodiscard]] OfflineRenderBoundaryStatus status();

private:
    [[nodiscard]] int                                     targetTick(ExportFramePlan const& frame) const;
    [[nodiscard]] std::optional<OfflineRenderClockSample> clockSample(ExportFramePlan const& frame) const;
    [[nodiscard]] OfflineRenderStepResult                 advanceWarmup(ExportFramePlan const& frame);
    [[nodiscard]] bool                                    recoverDownloadFailure(FrameDownloadQueueStatus const& status);
    [[nodiscard]] bool                                    publishClockSample(ExportFramePlan const& frame);
    void                                                  clearClockSample();
    void                                                  fault(OfflineRenderBoundaryError error, std::string message);

    functions::ReplaySession&                        mReplay;
    SaveableFramebufferQueue                         mDownloads;
    OfflineRenderFrameExecutor                       mExecutor;
    std::optional<ExportFramePlan>                   mPendingFrame;
    std::optional<ExportFramePlan>                   mLastSubmittedFrame;
    std::optional<functions::render::FrameTicket>    mCompletedFrameTicket;
    std::optional<functions::OfflineReplayTickToken> mReplayTickToken;
    std::optional<OfflineRenderClockToken>           mClockToken;
    int64_t                                          mMaximumReplayTick{};
    uint32_t                                         mCaptureCapacity{};
    uint32_t                                         mCaptureRetryCount{};
    uint32_t                                         mWarmupFramesRemaining{};
    uint32_t                                         mRenderWaitFrames{};
    bool                                             mTickGateOpen{};
    bool                                             mTimelineInitialized{};
    bool                                             mInitializationTickObserved{};
    OfflineRenderBoundaryState                       mState{OfflineRenderBoundaryState::Closed};
    OfflineRenderBoundaryError                       mError{OfflineRenderBoundaryError::None};
    std::string                                      mMessage;
};

} // namespace playback::editor::exporting
