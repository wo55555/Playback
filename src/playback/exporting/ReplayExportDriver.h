#pragma once

#include "ExportCoordinator.h"
#include "OfflineRenderBoundary.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>

namespace playback::replay {
class ReplaySession;
}

namespace playback::exporting {

class ReplayExportDriver {
public:
    ReplayExportDriver(ExportCoordinator& coordinator, replay::ReplaySession& replay);
    ~ReplayExportDriver();

    ReplayExportDriver(ReplayExportDriver const&)            = delete;
    ReplayExportDriver& operator=(ReplayExportDriver const&) = delete;

    // Export needs the ImGui renderer alive to capture at Present, so the boundary only exists while it is up.
    void setRendererAvailable(bool available);

    [[nodiscard]] bool start(
        ExportSettings                               settings,
        state::editing::model::EditorStateExt const& project,
        std::optional<std::string>                   cameraFallback = std::nullopt
    );
    void tick();
    void cancel();
    void reset();

    [[nodiscard]] bool isAvailable() const;
    [[nodiscard]] bool isActive() const;

private:
    enum class Phase : uint8_t {
        Idle,
        Rendering,
        Draining,
        Finalizing,
        Cancelling,
        Faulted,
        Completed,
        Cancelled,
    };

    enum class SubmissionResult : uint8_t { Ready, Backpressured, Failed };

    [[nodiscard]] SubmissionResult submitReadyFrames();
    [[nodiscard]] SubmissionResult collectDownloads();
    // Records a wait and keeps the renderer drawing for its duration, so a waiting frame is never a render-free frame.
    void                      waitFor(OfflineRenderTraceScope& trace, OfflineRenderWaitReason reason, uint64_t result);
    [[nodiscard]] ExportError mapBoundaryError(OfflineRenderBoundaryError error) const;
    void                      finish();
    void                      fail(ExportError error, std::string message);
    void                      restoreReplayState();
    void                      closeCapture(bool cancelled);
    void                      recordWait(OfflineRenderWaitReason reason) const noexcept;
    // Charges elapsed time to whichever reason the driver was parked on, so a frame's cost splits by cause.
    void accumulateWait(std::chrono::steady_clock::time_point now);
    void reportWaitProfile() const;

    ExportCoordinator&                     mCoordinator;
    replay::ReplaySession&                 mReplay;
    std::unique_ptr<OfflineRenderBoundary> mRenderBoundary;
    std::optional<CompiledExportPlan>      mPlan;
    std::deque<visuals::CapturedFrame>     mReadyFrames;
    std::optional<bool>                    mPreviousPaused;
    uint64_t                               mNextFrameIndex{};
    std::chrono::steady_clock::time_point  mWaitStartedAt{};
    std::optional<OfflineRenderWaitReason> mWaitReason;
    static constexpr size_t                WaitReasonCount = static_cast<size_t>(OfflineRenderWaitReason::Count);
    std::array<uint64_t, WaitReasonCount>  mWaitMicros{};
    std::array<uint64_t, WaitReasonCount>  mWaitHits{};
    std::chrono::steady_clock::time_point  mWaitChargedAt{};
    std::optional<OfflineRenderWaitReason> mWaitCharged;
    uint64_t                               mDriverTicks{};
    Phase                                  mPhase{Phase::Idle};
};

} // namespace playback::exporting
