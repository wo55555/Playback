#include "ReplayBrowserWindow.h"

#include "playback/refactor/editor/EditorTheme.h"
#include "playback/refactor/editor/iconfont.h"
#include "playback/editor/renderer/ImGuiRenderer.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <windows.h>
#include <commdlg.h>

namespace playback::refactor::replay_browser {

namespace {

constexpr float kNavigationHeight = 72.0f;
constexpr float kActionBarHeight  = 84.0f;
constexpr float kCardMinWidth     = 280.0f;
constexpr float kCardGap          = 18.0f;
constexpr float kButtonHeight     = 42.0f;

std::string formatSize(std::uintmax_t bytes) {
    constexpr std::array<char const*, 4> units{"B", "KB", "MB", "GB"};
    double size = static_cast<double>(bytes);
    size_t unit = 0;
    while (size >= 1024.0 && unit + 1 < units.size()) { size /= 1024.0; ++unit; }
    return unit == 0 ? std::to_string(bytes) + " " + units[unit]
                     : std::to_string(static_cast<int>(std::round(size * 10.0)) / 10.0) + " " + units[unit];
}

std::string formatDuration(screen::ReplaySummary const& replay) {
    int seconds = (replay.totalTicks > 0 ? replay.totalTicks : replay.durationTicks) / 20;
    return std::to_string(seconds / 60) + " 分 " + std::to_string(seconds % 60) + " 秒";
}

char const* sortLabel(screen::ReplaySort sort) {
    switch (sort) {
    case screen::ReplaySort::ReplayName: return "名称";
    case screen::ReplaySort::WorldName: return "世界";
    case screen::ReplaySort::Duration: return "时长";
    case screen::ReplaySort::FileSize: return "大小";
    default: return "日期";
    }
}

void tooltip(char const* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
}

} // namespace

ReplayBrowserWindow& ReplayBrowserWindow::getInstance() {
    static ReplayBrowserWindow instance;
    return instance;
}

void ReplayBrowserWindow::open() { refresh(); mOpen = true; }

void ReplayBrowserWindow::close() {
    mOpen = false;
    mSelectedId.clear();
    mShowDeleteDialog = false;
    playback::editor::renderer::gImGuiRenderer.clearReplayThumbnailTextures();
}

bool ReplayBrowserWindow::isOpen() const { return mOpen; }
bool ReplayBrowserWindow::ownsInput() const { return mOpen; }

void ReplayBrowserWindow::refresh() {
    auto selected = mSelectedId;
    mReplays = screen::ReplayBrowser::loadReplays();
    playback::editor::renderer::gImGuiRenderer.clearReplayThumbnailTextures();
    screen::ReplayBrowser::sortReplays(mReplays, mSort, mDescending);
    rebuildVisible();
    if (std::none_of(mReplays.begin(), mReplays.end(), [&](auto const& replay) { return replay.replayId == selected; })) {
        mSelectedId.clear();
    }
}

void ReplayBrowserWindow::rebuildVisible() {
    mVisible.clear();
    mVisible.reserve(mReplays.size());
    for (std::size_t index = 0; index < mReplays.size(); ++index) {
        if (mReplays[index].matches(mSearch)) mVisible.push_back(index);
    }
}

void ReplayBrowserWindow::select(std::string_view replayId) { mSelectedId.assign(replayId); }

std::optional<screen::ReplaySummary const*> ReplayBrowserWindow::selectedReplay() const {
    auto it = std::find_if(mReplays.begin(), mReplays.end(), [&](auto const& replay) { return replay.replayId == mSelectedId; });
    return it == mReplays.end() ? std::nullopt : std::optional<screen::ReplaySummary const*>{&*it};
}

void ReplayBrowserWindow::openSelected() {
    auto replay = selectedReplay();
    if (replay && (*replay)->canOpen && screen::ReplayBrowser::openReplay(**replay)) close();
}

void ReplayBrowserWindow::importReplay() {
    std::array<wchar_t, 32768> file{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"Replay files (*.playback;*.zip)\0*.playback;*.zip\0\0";
    dialog.lpstrFile = file.data();
    dialog.nMaxFile = static_cast<DWORD>(file.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) return;
    if (screen::ReplayBrowser::importReplay(file.data(), mOperationError)) refresh();
}

void ReplayBrowserWindow::draw() {
    if (!mOpen) return;
    editor::EditorTheme theme;
    theme.apply();
    auto const& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.bgPanel);
    ImGui::Begin("##replay-browser", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(1.15f);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
    drawNavigation();
    if (mViewMode == ViewMode::Grid) drawGrid(); else drawDetails();
    if (!mSelectedId.empty() && mViewMode == ViewMode::Grid) drawActionBar();
    drawDeleteDialog();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::End();
    ImGui::PopStyleColor();
}

void ReplayBrowserWindow::drawNavigation() {
    ImGui::BeginChild("##header", {0.0f, kNavigationHeight}, false);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {14.0f, 10.0f});
    if (ImGui::Button(ICON_PANEL_LEFT "  回放", {124.0f, kButtonHeight})) close();
    tooltip("返回主菜单");
    ImGui::SameLine(0.0f, 18.0f);
    std::array<char, 256> search{};
    std::copy_n(mSearch.data(), std::min(mSearch.size(), search.size() - 1), search.data());
    ImGui::SetNextItemWidth(std::clamp(ImGui::GetWindowWidth() * 0.28f, 260.0f, 420.0f));
    if (ImGui::InputTextWithHint("##search", ICON_SEARCH "  搜索回放或世界", search.data(), search.size())) { mSearch = search.data(); rebuildVisible(); }
    ImGui::SameLine();
    if (ImGui::Button(ICON_REFRESH, {kButtonHeight, kButtonHeight})) refresh();
    tooltip("刷新回放目录");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FILE_PLUS, {kButtonHeight, kButtonHeight})) importReplay();
    tooltip("导入回放文件");
    ImGui::SameLine();
    if (ImGui::BeginCombo("##sort", (std::string(ICON_SETTINGS) + "  " + sortLabel(mSort)).c_str())) {
        for (auto sort : {screen::ReplaySort::LastModified, screen::ReplaySort::ReplayName, screen::ReplaySort::WorldName, screen::ReplaySort::Duration, screen::ReplaySort::FileSize}) {
            bool selected = mSort == sort;
            if (ImGui::Selectable(sortLabel(sort), selected)) { mSort = sort; screen::ReplayBrowser::sortReplays(mReplays, mSort, mDescending); rebuildVisible(); }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(mDescending ? "降序" : "升序")) { mDescending = !mDescending; screen::ReplayBrowser::sortReplays(mReplays, mSort, mDescending); rebuildVisible(); }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_GRID, {kButtonHeight, kButtonHeight})) mViewMode = ViewMode::Grid;
    tooltip("平铺视图");
    ImGui::SameLine();
    if (ImGui::Button(ICON_LIST, {kButtonHeight, kButtonHeight})) mViewMode = ViewMode::Details;
    tooltip("详细信息视图");
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::Separator();
}

void ReplayBrowserWindow::drawPreview(screen::ReplaySummary const& replay, ImVec2 size) {
    auto start = ImGui::GetCursorScreenPos();
    auto end = ImVec2(start.x + size.x, start.y + size.y);
    ImGui::GetWindowDrawList()->AddRectFilled(start, end, IM_COL32(30, 42, 58, 255), 8.0f);
    auto texture = playback::editor::renderer::gImGuiRenderer.acquireReplayThumbnailTexture(replay.path.string(), replay.thumbnailPng);
    if (texture) {
        ImGui::Image(texture, size);
    } else {
        ImGui::GetWindowDrawList()->AddRect(start, end, IM_COL32(58, 140, 240, 130), 8.0f, 0, 1.0f);
        ImGui::SetCursorPos({ImGui::GetCursorPosX() + 18.0f, ImGui::GetCursorPosY() + size.y * 0.42f});
        ImGui::TextDisabled("暂无缩略图");
        ImGui::Dummy(size);
    }
}

void ReplayBrowserWindow::drawCard(screen::ReplaySummary const& replay, float width) {
    bool const selected = replay.replayId == mSelectedId;
    ImGui::PushID(replay.replayId.c_str());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? IM_COL32(30, 58, 94, 255) : IM_COL32(37, 37, 37, 255));
    ImGui::BeginChild("##card", {width, width * 0.75f + 128.0f}, true);
    drawPreview(replay, {ImGui::GetContentRegionAvail().x, width * 0.75f - 8.0f});
    if (ImGui::Selectable(replay.displayName().c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
        select(replay.replayId);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openSelected();
    }
    ImGui::TextDisabled("%s", replay.worldName.empty() ? "未知世界" : replay.worldName.c_str());
    ImGui::TextDisabled("%s   %s", formatDuration(replay).c_str(), formatSize(replay.fileSize).c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

void ReplayBrowserWindow::drawGrid() {
    float actionHeight = mSelectedId.empty() ? 0.0f : kActionBarHeight;
    ImGui::BeginChild("##grid", {0.0f, -actionHeight}, false);
    ImGui::TextDisabled("%zu 个回放文件", mVisible.size());
    if (mVisible.empty()) { ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 48.0f); ImGui::TextDisabled("没有匹配的回放文件"); ImGui::EndChild(); return; }
    float available = ImGui::GetContentRegionAvail().x;
    int columns = std::max(1, static_cast<int>((available + kCardGap) / (kCardMinWidth + kCardGap)));
    float width = std::min(380.0f, (available - (columns - 1) * kCardGap) / columns);
    int first = std::max(0, static_cast<int>(ImGui::GetScrollY() / (width * 0.75f + 146.0f)) * columns - columns);
    int last = std::min(static_cast<int>(mVisible.size()), first + (static_cast<int>(ImGui::GetWindowHeight() / (width * 0.75f + 146.0f)) + 4) * columns);
    if (first > 0) ImGui::Dummy({0.0f, static_cast<float>(first / columns) * (width * 0.75f + 146.0f)});
    for (int item = first; item < last; ++item) {
        drawCard(mReplays[mVisible[static_cast<size_t>(item)]], width);
        if ((item + 1) % columns != 0) ImGui::SameLine(0.0f, kCardGap);
    }
    int remainingRows = static_cast<int>((mVisible.size() - static_cast<size_t>(last) + columns - 1) / columns);
    if (remainingRows > 0) ImGui::Dummy({0.0f, remainingRows * (width * 0.75f + 146.0f)});
    ImGui::EndChild();
}

void ReplayBrowserWindow::drawDetails() {
    ImGui::BeginChild("##details-list", {std::max(320.0f, ImGui::GetContentRegionAvail().x * 0.34f), 0.0f}, true);
    for (auto index : mVisible) {
        auto const& replay = mReplays[index];
        bool selected = replay.replayId == mSelectedId;
        if (ImGui::Selectable(replay.displayName().c_str(), selected, 0, {0.0f, 54.0f})) select(replay.replayId);
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextDisabled("%s", replay.worldName.c_str());
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##details-preview", {0.0f, 0.0f}, true);
    auto replay = selectedReplay();
    if (!replay) { ImGui::TextDisabled("选择一个回放以查看详细信息"); ImGui::EndChild(); return; }
    float previewWidth = ImGui::GetContentRegionAvail().x;
    drawPreview(**replay, {previewWidth, previewWidth * 9.0f / 16.0f});
    ImGui::Separator();
    ImGui::Text("%s", (*replay)->displayName().c_str());
    ImGui::TextDisabled("世界  %s", (*replay)->worldName.empty() ? "未知" : (*replay)->worldName.c_str());
    ImGui::TextDisabled("时长  %s", formatDuration(**replay).c_str());
    ImGui::TextDisabled("大小  %s", formatSize((*replay)->fileSize).c_str());
    ImGui::TextDisabled("文件  %s", (*replay)->replayId.c_str());
    ImGui::Spacing();
    if (ImGui::Button(ICON_OPEN "  打开回放", {160.0f, kButtonHeight})) openSelected();
    ImGui::SameLine();
    if (ImGui::Button(ICON_DELETE "  删除", {140.0f, kButtonHeight})) mShowDeleteDialog = true;
    ImGui::SameLine();
    if (ImGui::Button(ICON_OPEN, {kButtonHeight, kButtonHeight})) {
        auto const shown = screen::ReplayBrowser::showInFolder(**replay);
        static_cast<void>(shown);
    }
    tooltip("在文件夹中显示");
    ImGui::EndChild();
}

void ReplayBrowserWindow::drawActionBar() {
    auto replay = selectedReplay();
    if (!replay) return;
    ImGui::BeginChild("##actions", {0.0f, kActionBarHeight}, true);
    ImGui::Text("%s", (*replay)->displayName().c_str());
    ImGui::TextDisabled("%s  ·  %s", formatDuration(**replay).c_str(), formatSize((*replay)->fileSize).c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - 390.0f);
    if (ImGui::Button(ICON_OPEN "  打开", {120.0f, kButtonHeight})) openSelected();
    ImGui::SameLine();
    if (ImGui::Button(ICON_SETTINGS "  编辑", {120.0f, kButtonHeight})) openSelected();
    ImGui::SameLine();
    if (ImGui::Button(ICON_DELETE, {kButtonHeight, kButtonHeight})) mShowDeleteDialog = true;
    tooltip("删除回放");
    ImGui::EndChild();
}

void ReplayBrowserWindow::drawDeleteDialog() {
    if (mShowDeleteDialog) ImGui::OpenPopup("删除回放");
    if (ImGui::BeginPopupModal("删除回放", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        auto replay = selectedReplay();
        ImGui::Text("确定删除选中的回放文件吗");
        ImGui::TextDisabled("此操作无法撤销");
        ImGui::Spacing();
        if (ImGui::Button(ICON_DELETE "  确认删除", {156.0f, kButtonHeight}) && replay) {
            auto const deleted = screen::ReplayBrowser::deleteReplay(**replay, mOperationError);
            static_cast<void>(deleted);
            mShowDeleteDialog = false;
            ImGui::CloseCurrentPopup();
            refresh();
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_CLOSE "  取消", {120.0f, kButtonHeight})) { mShowDeleteDialog = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (!mOperationError.empty()) {
        ImGui::OpenPopup("回放操作失败");
        if (ImGui::BeginPopupModal("回放操作失败", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", mOperationError.c_str());
            if (ImGui::Button(ICON_CHECK "  确定", {120.0f, kButtonHeight})) { mOperationError.clear(); ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
}

} // namespace playback::refactor::replay_browser
