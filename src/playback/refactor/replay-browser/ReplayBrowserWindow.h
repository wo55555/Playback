#pragma once

#include "playback/screen/ReplayBrowser.h"

#include "imgui.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace playback::refactor::replay_browser {

enum class ReplayFilter { All, Playable, Broken };

class ReplayBrowserWindow {
public:
    static ReplayBrowserWindow& getInstance();

    void open();
    void close();
    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] bool ownsInput() const;
    void draw();

private:
    enum class ViewMode { Grid, Details };

    void refresh();
    void rebuildVisible();
    void drawNavigation();
    void drawGrid();
    void drawDetails();
    void drawCard(screen::ReplaySummary const& replay, std::size_t visibleIndex, float width);
    void drawPreview(screen::ReplaySummary const& replay, ImVec2 size);
    void drawActionBar();
    void drawDeleteDialog();
    void drawRenameDialog();
    void openRenameDialog();
    void select(std::string_view replayId, std::size_t visibleIndex, bool toggle, bool range);
    void openSelected();
    void importReplay();
    [[nodiscard]] std::optional<screen::ReplaySummary const*> selectedReplay() const;

    bool                               mOpen{};
    std::vector<screen::ReplaySummary> mReplays;
    std::vector<std::size_t>           mVisible;
    std::unordered_set<std::string>    mSelectedIds;
    std::optional<std::size_t>         mSelectionAnchor;
    std::string                        mSearch;
    screen::ReplaySort                 mSort = screen::ReplaySort::LastModified;
    bool                               mDescending = true;
    ReplayFilter                       mFilter = ReplayFilter::All;
    ViewMode                           mViewMode = ViewMode::Grid;
    bool                               mShowDeleteDialog{};
    bool                               mRenameDialogOpen{};
    std::string                        mRenameBuffer;
    std::string                        mOperationError;
    double                             mInfoHoverStart = -1.0; // 卡片信息图标悬停起始时间，用于 tooltip 淡入
};

} // namespace playback::refactor::replay_browser
