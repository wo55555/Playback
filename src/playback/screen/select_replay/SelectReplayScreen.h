#pragma once

#include "playback/state/EditorAction.h"
#include "playback/state/ReplayBrowserState.h"

#include "imgui.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace playback::screen::select_replay {

enum class ReplayFilter { All, Playable, Broken };
enum class BrowserSort { LastModified, ReplayName, WorldName, Duration, FileSize };

class SelectReplayScreen {
public:
    using SubmitAction = std::function<void(playback::state::EditorAction)>;

    static SelectReplayScreen& getInstance();

    void draw(playback::state::ReplayBrowserState const& state, SubmitAction const& submit);

private:
    enum class ViewMode { Grid, Details };

    void syncSnapshot();
    void rebuildVisible();
    void drawNavigation();
    void drawGrid();
    void drawDetails();
    void drawDetailsListItem(playback::state::ReplayBrowserEntry const& replay, std::size_t visibleIndex, float width);
    void drawCard(playback::state::ReplayBrowserEntry const& replay, std::size_t visibleIndex, float width);
    void drawPreview(playback::state::ReplayBrowserEntry const& replay, ImVec2 size);
    void drawActionBar();
    void drawDeleteDialog();
    void drawRenameDialog();
    void openRenameDialog();
    void select(std::string_view replayId, std::size_t visibleIndex, bool toggle, bool range);
    void openSelected();
    void importReplay();
    [[nodiscard]] std::vector<playback::state::ReplayBrowserEntry> const&   replays() const;
    [[nodiscard]] std::optional<playback::state::ReplayBrowserEntry const*> selectedReplay() const;
    void submit(playback::state::EditorAction action) const;

    playback::state::ReplayBrowserState const* mState{};
    SubmitAction const*                         mSubmit{};
    std::uint64_t                               mSnapshotRevision{};
    std::vector<std::size_t>                    mVisible;
    std::unordered_set<std::string>             mSelectedIds;
    std::optional<std::size_t>                  mSelectionAnchor;
    std::string                                 mSearch;
    BrowserSort                                 mSort       = BrowserSort::LastModified;
    bool                                        mDescending = true;
    ReplayFilter                                mFilter     = ReplayFilter::All;
    ViewMode                                    mViewMode   = ViewMode::Grid;
    bool                                        mShowDeleteDialog{};
    bool                                        mRenameDialogOpen{};
    std::string                                 mRenameBuffer;
};

} // namespace playback::screen::select_replay
