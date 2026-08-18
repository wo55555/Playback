#pragma once

#include "playback/exporting/ExportCoordinator.h"
#include "playback/exporting/ReplayExportDriver.h"
#include "playback/state/EditorContext.h"
#include "playback/state/editing/commands/CommandStack.h"
#include "playback/state/editing/models/SelectionModel.h"


#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace playback::visuals {
class FrameTap;
}

namespace playback::state {

class EditorController {
public:
    explicit EditorController(EditorContext& context);
    ~EditorController();

    void setFrameTap(visuals::FrameTap* frameTap);
    void reset();
    void tickExportBeforeClientUpdate();
    void tick(bool hudVisible);

private:
    void publishState(bool hudVisible);
    void publishCameraTimeline();
    void ensureProject(int totalTicks, std::string_view replayPath);
    void applyEditorAction(EditorAction const& action);
    [[nodiscard]] std::optional<state::editing::model::CameraKeyframe> captureCameraKeyframe() const;
    void                                                               refreshBrowser();
    void runBrowserOperation(ReplayBrowserOperation operation, bool hudVisible, auto&& callback) {
        mBrowserOperation = operation;
        publishState(hudVisible);
        callback();
        mBrowserOperation = ReplayBrowserOperation::None;
    }

    [[nodiscard]] ReplayBrowserEntry const* findBrowserEntry(std::string_view replayId) const;

    EditorContext&                                 mContext;
    bool                                           mBrowserVisible{};
    std::uint64_t                                  mBrowserRevision{};
    ReplayBrowserOperation                         mBrowserOperation{ReplayBrowserOperation::None};
    std::string                                    mBrowserError;
    std::shared_ptr<ReplayBrowserSnapshot const>   mBrowserSnapshot;
    state::editing::model::EditorStateExt          mProject;
    state::editing::command::CommandStack          mCommandStack;
    exporting::ExportCoordinator                   mExportCoordinator;
    std::unique_ptr<exporting::ReplayExportDriver> mExportDriver;
    std::string                                    mActiveReplayPath;
    std::optional<std::string>                     mPreviewCameraId;
    int                                            mProjectTotalTicks{-1};
    bool                                           mExportTickedBeforeClientUpdate{};
};

} // namespace playback::state
