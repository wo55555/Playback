#pragma once

#include "ExportTypes.h"
#include "OfflineRenderClockHooks.h"
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
    PreparingReplay,
    AwaitingRender,
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
    OfflineRenderBoundaryState state{OfflineRenderBoundaryState::Closed};
    OfflineRenderBoundaryError error{OfflineRenderBoundaryError::None};
    std::string                message;
    FrameDownloadQueueStatus   downloads;
};

// Owns the boundary between export replay-sample preparation and the renderer.
// The replay sample cannot advance until FrameTap confirms that the matching
// rendered frame has entered a GPU download slot.
class OfflineRenderBoundary {
public:
    OfflineRenderBoundary(functions::ReplaySession& replay, functions::render::FrameTap& frameTap);
    ~OfflineRenderBoundary();

    OfflineRenderBoundary(OfflineRenderBoundary const&)            = delete;
    OfflineRenderBoundary& operator=(OfflineRenderBoundary const&) = delete;

    [[nodiscard]] bool open(uint32_t capacity, int64_t maximumReplayTick);
    void               close();
    void               cancel();

    [[nodiscard]] OfflineRenderStepResult advance(ExportFramePlan const& frame);
    [[nodiscard]] bool                    beginDrain();
    [[nodiscard]] bool                    isDrained();

    [[nodiscard]] std::optional<functions::render::CapturedFrame> finishDownload();

    [[nodiscard]] OfflineRenderBoundaryStatus status();

private:
    [[nodiscard]] int                                     targetTick(ExportFramePlan const& frame) const;
    [[nodiscard]] std::optional<OfflineRenderClockSample> clockSample(ExportFramePlan const& frame) const;
    void                                                  clearClockSample();
    void                                                  fault(OfflineRenderBoundaryError error, std::string message);

    functions::ReplaySession&                        mReplay;
    SaveableFramebufferQueue                         mDownloads;
    std::optional<ExportFramePlan>                   mPendingFrame;
    std::optional<ExportFramePlan>                   mLastSubmittedFrame;
    std::optional<functions::OfflineReplayTickToken> mReplayTickToken;
    std::optional<OfflineRenderClockToken>           mClockToken;
    int64_t                                          mMaximumReplayTick{};
    bool                                             mTickGateOpen{};
    OfflineRenderBoundaryState                       mState{OfflineRenderBoundaryState::Closed};
    OfflineRenderBoundaryError                       mError{OfflineRenderBoundaryError::None};
    std::string                                      mMessage;
};

} // namespace playback::editor::exporting
