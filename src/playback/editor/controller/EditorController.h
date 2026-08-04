#pragma once

#include "playback/editor/context/EditorContext.h"
#include "playback/editor/editing/commands/CommandStack.h"
#include "playback/editor/editing/models/SelectionModel.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace playback::editor {

class EditorController {
public:
    explicit EditorController(EditorContext& context);

    void reset();
    void tick(bool hudVisible);

private:
    void publishState(bool hudVisible);
    void ensureProject(int totalTicks, std::string_view replayPath);
    void applyEditorAction(EditorAction const& action);
    void refreshBrowser();
    void runBrowserOperation(ReplayBrowserOperation operation, bool hudVisible, auto&& callback) {
        mBrowserOperation = operation;
        publishState(hudVisible);
        callback();
        mBrowserOperation = ReplayBrowserOperation::None;
    }

    [[nodiscard]] ReplayBrowserEntry const* findBrowserEntry(std::string_view replayId) const;

    EditorContext&                               mContext;
    bool                                         mBrowserVisible{};
    std::uint64_t                                mBrowserRevision{};
    ReplayBrowserOperation                       mBrowserOperation{ReplayBrowserOperation::None};
    std::string                                  mBrowserError;
    std::shared_ptr<ReplayBrowserSnapshot const> mBrowserSnapshot;
    editing::model::EditorStateExt               mProject;
    editing::command::CommandStack               mCommandStack;
    std::string                                  mActiveReplayPath;
    int                                          mProjectTotalTicks{-1};
};

} // namespace playback::editor
