#pragma once

#include "ExportCoordinator.h"
#include "OfflineRenderBoundary.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>

namespace playback::functions {
class ReplaySession;
}

namespace playback::editor::exporting {

class ReplayExportDriver {
public:
    ReplayExportDriver(ExportCoordinator& coordinator, functions::ReplaySession& replay);
    ~ReplayExportDriver();

    ReplayExportDriver(ReplayExportDriver const&)            = delete;
    ReplayExportDriver& operator=(ReplayExportDriver const&) = delete;

    void setFrameTap(functions::render::FrameTap* frameTap);

    [[nodiscard]] bool start(ExportSettings settings, editing::model::EditorStateExt const& project);
    void               tick();
    void               cancel();
    void               reset();

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
    [[nodiscard]] ExportError      mapBoundaryError(OfflineRenderBoundaryError error) const;
    void                           finish();
    void                           fail(ExportError error, std::string message);
    void                           restoreReplayState();
    void                           closeCapture(bool cancelled);

    ExportCoordinator&                           mCoordinator;
    functions::ReplaySession&                    mReplay;
    std::unique_ptr<OfflineRenderBoundary>       mRenderBoundary;
    std::optional<CompiledExportPlan>            mPlan;
    std::deque<functions::render::CapturedFrame> mReadyFrames;
    bool                                         mPreviousPaused{};
    bool                                         mRestorePaused{};
    uint64_t                                     mNextFrameIndex{};
    Phase                                        mPhase{Phase::Idle};
};

} // namespace playback::editor::exporting
