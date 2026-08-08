#pragma once

#include "playback/editor/context/EditorAction.h"
#include "playback/editor/context/ReplayBrowserState.h"

#include "imgui.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace playback::screen::select_replay {

enum class ReplayFilter { All, Playable, Broken };
enum class BrowserSort { LastModified, ReplayName, WorldName, Duration, FileSize };

class SelectReplayScreen {
public:
    using SubmitAction = std::function<void(playback::editor::EditorAction)>;

    static SelectReplayScreen& getInstance();

    void draw(playback::editor::ReplayBrowserState const& state, SubmitAction const& submit);

private:
    enum class ViewMode { Grid, Details };

    void syncSnapshot();
    void rebuildVisible();
    void drawNavigation();
    void drawGrid();
    void drawDetails();
    void drawDetailsListItem(playback::editor::ReplayBrowserEntry const& replay, std::size_t visibleIndex, float width);
    void drawCard(playback::editor::ReplayBrowserEntry const& replay, std::size_t visibleIndex, float width);
    void drawPreview(playback::editor::ReplayBrowserEntry const& replay, ImVec2 size, float rounding);
    void drawActionBar();
    void drawDeleteDialog();
    void drawRenameDialog();
    void openRenameDialog();
    void select(std::string_view replayId, std::size_t visibleIndex, bool toggle, bool range);
    void openSelected();
    void importReplay();
    void updateAnimations();
    float animate(std::string_view key, float target);
    [[nodiscard]] std::vector<playback::editor::ReplayBrowserEntry> const&   replays() const;
    [[nodiscard]] std::optional<playback::editor::ReplayBrowserEntry const*> selectedReplay() const;
    void submit(playback::editor::EditorAction action) const;

    playback::editor::ReplayBrowserState const* mState{};
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
    std::unordered_map<std::string, float>      mAnimationValues;
    float                                       mViewTransition{};
    bool                                        mViewTransitionActive{};
};

} // namespace playback::screen::select_replay
