#pragma once

#include "playback/exporting/ExportCoordinator.h"
#include "playback/exporting/ReplayExportDriver.h"
#include "playback/state/EditorContext.h"
#include "playback/state/editing/commands/CommandStack.h"
#include "playback/state/editing/models/SelectionModel.h"


#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace playback::state {

class EditorController {
public:
    explicit EditorController(EditorContext& context);
    ~EditorController();

    void setRendererAvailable(bool available);
    void reset();
    void tickExportBeforeClientUpdate();
    void tickExportDuringGraphics();
    void tick(bool hudVisible);

private:
    void               publishState(bool hudVisible);
    void               publishCameraTimeline();
    void               ensureProject(int totalTicks, std::string_view replayPath);
    void               applyEditorAction(EditorAction const& action);
    void               loadProjectForReplay(std::string_view replayPath);
    bool               saveProject(std::filesystem::path const& path);
    void               autosaveIfDue();
    void               flushProjectOnClose();
    [[nodiscard]] bool isProjectDirty() const { return mCommandStack.revision() != mSavedRevision; }
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
    bool                                           mExportTickReentered{};

    std::filesystem::path                 mProjectFile;
    std::string                           mProjectError;
    std::uint64_t                         mSavedRevision{};
    std::chrono::steady_clock::time_point mLastAutosave{};
    bool                                  mEditorVisibleLogged{};
    bool                                  mEditorReadyLatched{};
};

} // namespace playback::state
