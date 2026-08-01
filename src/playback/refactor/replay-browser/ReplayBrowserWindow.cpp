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
#include <iomanip>
#include <sstream>
#include <windows.h>
#include <commdlg.h>

namespace playback::refactor::replay_browser {

namespace {

constexpr float kNavHeight        = 84.0f;
constexpr float kActionBarHeight  = 104.0f;
constexpr float kScreenMargin     = 50.0f;
constexpr float kCardGap          = 24.0f;
constexpr float kCardPadding      = 16.0f;
constexpr float kControlHeight    = 52.0f;
constexpr float kFontScaleHeader  = 1.6f;
constexpr float kFontScaleBody    = 1.0f;
constexpr float kFontScaleSmall   = 0.9f;
constexpr float kFontScaleCardTitle = 2.0f;
constexpr float kFontScaleCardMeta  = 1.6f;
constexpr float kFontScaleActionTitle = 3.0f;
constexpr float kFontScaleActionMeta  = 2.0f;

constexpr ImU32 kColorAccent       = IM_COL32(58,  140, 240, 255);
constexpr ImU32 kColorAccentDim    = IM_COL32(58,  140, 240, 130);
constexpr ImU32 kColorBg           = IM_COL32(28,  28,  28,  255);
constexpr ImU32 kColorCardBg       = IM_COL32(48,  48,  48,  255);
constexpr ImU32 kColorCardSelected = IM_COL32(85,  85,  85,  255);
constexpr ImU32 kColorButton       = IM_COL32(55,  55,  55,  255);
constexpr ImU32 kColorButtonHover  = IM_COL32(70,  70,  70,  255);
constexpr ImU32 kColorButtonActive = IM_COL32(90,  90,  90,  255);
constexpr ImU32 kColorPreviewBg    = IM_COL32(30,  42,  58,  255);
constexpr ImU32 kColorDanger       = IM_COL32(210, 60,  60,  255);
constexpr ImU32 kColorText         = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kColorTextDim      = IM_COL32(180, 180, 180, 255);

std::string formatSize(std::uintmax_t bytes) {
    constexpr std::array<char const*, 4> units{"B", "KB", "MB", "GB"};
    double size = static_cast<double>(bytes);
    size_t unit = 0;
    while (size >= 1024.0 && unit + 1 < units.size()) { size /= 1024.0; ++unit; }
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1) << size << ' ' << units[unit];
    return stream.str();
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

void pushTextColor(ImU32 col) { ImGui::PushStyleColor(ImGuiCol_Text, col); }

void styleButton(bool active = false) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, kColorButtonActive);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorButtonActive);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColorButtonActive);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, kColorButton);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorButtonHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColorButtonActive);
    }
}

void popButtonStyle() { ImGui::PopStyleColor(3); }

bool textButton(char const* label, float width, bool active = false) {
    styleButton(active);
    bool clicked = ImGui::Button(label, {width, kControlHeight});
    popButtonStyle();
    return clicked;
}

bool iconButton(char const* icon, float width, bool active = false) {
    styleButton(active);
    bool clicked = ImGui::Button(icon, {width, kControlHeight});
    popButtonStyle();
    return clicked;
}

} // namespace

ReplayBrowserWindow& ReplayBrowserWindow::getInstance() {
    static ReplayBrowserWindow instance;
    return instance;
}

void ReplayBrowserWindow::open() { refresh(); mOpen = true; }

void ReplayBrowserWindow::close() {
    mOpen = false;
    mSelectedIds.clear();
    mSelectionAnchor.reset();
    mShowDeleteDialog = false;
    playback::editor::renderer::gImGuiRenderer.clearReplayThumbnailTextures();
}

bool ReplayBrowserWindow::isOpen() const { return mOpen; }
bool ReplayBrowserWindow::ownsInput() const { return mOpen; }

void ReplayBrowserWindow::refresh() {
    mReplays = screen::ReplayBrowser::loadReplays();
    playback::editor::renderer::gImGuiRenderer.clearReplayThumbnailTextures();
    screen::ReplayBrowser::sortReplays(mReplays, mSort, mDescending);
    rebuildVisible();
    std::erase_if(mSelectedIds, [&](auto const& id) {
        return std::none_of(mReplays.begin(), mReplays.end(), [&](auto const& replay) { return replay.replayId == id; });
    });
}

void ReplayBrowserWindow::rebuildVisible() {
    mVisible.clear();
    mVisible.reserve(mReplays.size());
    for (std::size_t index = 0; index < mReplays.size(); ++index) {
        if (mReplays[index].matches(mSearch)) mVisible.push_back(index);
    }
}

void ReplayBrowserWindow::select(std::string_view replayId, std::size_t visibleIndex, bool toggle, bool range) {
    if (range && mSelectionAnchor && *mSelectionAnchor < mVisible.size()) {
        auto const first = std::min(*mSelectionAnchor, visibleIndex);
        auto const last  = std::max(*mSelectionAnchor, visibleIndex);
        for (auto index = first; index <= last; ++index) mSelectedIds.insert(mReplays[mVisible[index]].replayId);
    } else if (toggle) {
        if (!mSelectedIds.erase(std::string(replayId))) mSelectedIds.insert(std::string(replayId));
        mSelectionAnchor = visibleIndex;
    } else {
        mSelectedIds = {std::string(replayId)};
        mSelectionAnchor = visibleIndex;
    }
}

std::optional<screen::ReplaySummary const*> ReplayBrowserWindow::selectedReplay() const {
    if (mSelectedIds.size() != 1) return std::nullopt;
    auto it = std::find_if(mReplays.begin(), mReplays.end(), [&](auto const& replay) { return mSelectedIds.contains(replay.replayId); });
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
    dialog.lpstrFilter = L"Replay files (*.playback)\0*.playback\0\0";
    dialog.lpstrFile   = file.data();
    dialog.nMaxFile    = static_cast<DWORD>(file.size());
    dialog.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
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
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kColorBg);
    ImGui::Begin("##replay-browser", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);

    ImGui::SetWindowFontScale(kFontScaleBody);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) close();

    drawNavigation();
    ImGui::Separator();

    float const actionHeight = (mViewMode == ViewMode::Grid && !mSelectedIds.empty()) ? kActionBarHeight : 0.0f;
    ImGui::BeginChild("##content", {0.0f, -actionHeight}, false);
    if (mViewMode == ViewMode::Grid) drawGrid(); else drawDetails();
    ImGui::EndChild();

    if (mViewMode == ViewMode::Grid && !mSelectedIds.empty()) drawActionBar();

    drawDeleteDialog();

    ImGui::SetWindowFontScale(kFontScaleBody);
    ImGui::End();
    ImGui::PopStyleColor();
}

void ReplayBrowserWindow::drawNavigation() {
    ImGui::BeginChild("##header", {0.0f, kNavHeight}, false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::SetWindowFontScale(kFontScaleHeader);

    float const width     = ImGui::GetWindowWidth();
    float const margin    = 24.0f;
    float const totalGap  = 12.0f;
    float const iconW     = kControlHeight;
    float const textW     = 90.0f;
    float const filterW   = 130.0f;
    float const importW   = 110.0f;
    float const searchW   = 300.0f;
    float const controlsW = iconW + textW + textW + filterW + importW + searchW + totalGap * 6.0f;
    float const startX    = std::max(margin + 180.0f, width - margin - controlsW);

    // Back arrow left of title
    ImGui::SetCursorPos({margin, (kNavHeight - kControlHeight) * 0.5f});
    if (iconButton(ICON_BACK, iconW)) close();
    tooltip("返回主菜单");

    ImGui::SameLine(0.0f, 12.0f);
    ImGui::AlignTextToFramePadding();
    pushTextColor(kColorText);
    ImGui::TextUnformatted("回放列表");
    ImGui::PopStyleColor();

    float x = startX;

    // Search
    ImGui::SetCursorPos({x, (kNavHeight - kControlHeight) * 0.5f});
    ImGui::SetNextItemWidth(searchW);
    std::array<char, 256> search{};
    std::copy_n(mSearch.data(), std::min(mSearch.size(), search.size() - 1), search.data());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.0f, 12.0f});
    if (ImGui::InputTextWithHint("##search", ICON_SEARCH "  搜索回放文件", search.data(), search.size())) {
        mSearch = search.data();
        rebuildVisible();
    }
    ImGui::PopStyleVar();
    x += searchW + totalGap;

    // Import
    ImGui::SetCursorPos({x, (kNavHeight - kControlHeight) * 0.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.0f, 12.0f});
    styleButton();
    if (ImGui::Button(ICON_EXPORT "  导入", {importW, kControlHeight})) importReplay();
    popButtonStyle();
    ImGui::PopStyleVar();
    tooltip("导入回放文件");
    x += importW + totalGap;

    // Filter
    ImGui::SetCursorPos({x, (kNavHeight - kControlHeight) * 0.5f});
    ImGui::SetNextItemWidth(filterW);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.0f, 12.0f});
    styleButton();
    if (ImGui::BeginCombo("##filter", ICON_SORT "  过滤")) {
        popButtonStyle();
        for (auto sort : {screen::ReplaySort::LastModified, screen::ReplaySort::ReplayName,
                          screen::ReplaySort::WorldName, screen::ReplaySort::Duration, screen::ReplaySort::FileSize}) {
            bool selected = mSort == sort;
            if (ImGui::Selectable(sortLabel(sort), selected)) {
                mSort = sort;
                screen::ReplayBrowser::sortReplays(mReplays, mSort, mDescending);
                rebuildVisible();
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("升序", nullptr, !mDescending)) {
            mDescending = false;
            screen::ReplayBrowser::sortReplays(mReplays, mSort, mDescending);
            rebuildVisible();
        }
        if (ImGui::MenuItem("降序", nullptr, mDescending)) {
            mDescending = true;
            screen::ReplayBrowser::sortReplays(mReplays, mSort, mDescending);
            rebuildVisible();
        }
        ImGui::EndCombo();
    } else {
        popButtonStyle();
    }
    ImGui::PopStyleVar();
    x += filterW + totalGap;

    // View toggles
    ImGui::SetCursorPos({x, (kNavHeight - kControlHeight) * 0.5f});
    if (textButton(ICON_GRID "  平铺", textW, mViewMode == ViewMode::Grid)) mViewMode = ViewMode::Grid;
    tooltip("平铺视图");
    x += textW + totalGap;

    ImGui::SetCursorPos({x, (kNavHeight - kControlHeight) * 0.5f});
    if (textButton(ICON_LIST "  列表", textW, mViewMode == ViewMode::Details)) mViewMode = ViewMode::Details;
    tooltip("列表视图");
    x += textW + totalGap;

    // Settings
    ImGui::SetCursorPos({x, (kNavHeight - kControlHeight) * 0.5f});
    if (iconButton(ICON_SETTINGS, iconW)) ImGui::OpenPopup("##settings");
    tooltip("设置");
    if (ImGui::BeginPopup("##settings")) {
        ImGui::TextDisabled("回放目录设置将在后续版本提供");
        ImGui::EndPopup();
    }

    ImGui::EndChild();
}

void ReplayBrowserWindow::drawPreview(screen::ReplaySummary const& replay, ImVec2 size) {
    auto start = ImGui::GetCursorScreenPos();
    auto end   = ImVec2(start.x + size.x, start.y + size.y);
    ImGui::GetWindowDrawList()->AddRectFilled(start, end, kColorPreviewBg, 0.0f);

    auto texture = playback::editor::renderer::gImGuiRenderer.acquireReplayThumbnailTexture(replay.path.string(), replay.thumbnailPng);
    if (texture) {
        // 缩略图源固定为 16:9，按目标区域比例居中裁剪，绝不拉伸。
        constexpr float sourceAspect = 16.0f / 9.0f;
        float const targetAspect = size.x / size.y;
        ImVec2 uv0{0.0f, 0.0f};
        ImVec2 uv1{1.0f, 1.0f};
        if (targetAspect < sourceAspect) {
            float const visibleWidth = targetAspect / sourceAspect;
            uv0.x = (1.0f - visibleWidth) * 0.5f;
            uv1.x = 1.0f - uv0.x;
        } else if (targetAspect > sourceAspect) {
            float const visibleHeight = sourceAspect / targetAspect;
            uv0.y = (1.0f - visibleHeight) * 0.5f;
            uv1.y = 1.0f - uv0.y;
        }
        ImGui::Image(texture, size, uv0, uv1);
    } else {
        auto center = ImVec2(start.x + size.x * 0.5f, start.y + size.y * 0.5f);
        auto const* msg = "暂无缩略图";
        auto ts = ImGui::CalcTextSize(msg);
        ImGui::GetWindowDrawList()->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), kColorTextDim, msg);
        ImGui::Dummy(size);
    }
}

void ReplayBrowserWindow::drawCard(screen::ReplaySummary const& replay, std::size_t visibleIndex, float width) {
    bool const selected = mSelectedIds.contains(replay.replayId);
    float const cardHeight = width * 6.0f / 5.0f;

    ImGui::PushID(replay.replayId.c_str());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? kColorCardSelected : kColorCardBg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::BeginChild(
        "##card",
        {width, cardHeight},
        false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    // 卡片为 5:6，顶部缩略图始终严格保持 4:3。
    float const previewWidth  = width - kCardPadding * 2.0f;
    float const previewHeight = previewWidth * 3.0f / 4.0f;
    ImGui::SetCursorPos({kCardPadding, kCardPadding});
    drawPreview(replay, {previewWidth, previewHeight});

    // Clickable full-card overlay (after preview so it receives clicks over the whole card)
    ImGui::SetCursorPos({0.0f, 0.0f});
    if (ImGui::InvisibleButton("##select-card", {width, cardHeight})) {
        select(replay.replayId, visibleIndex, ImGui::GetIO().KeyCtrl, ImGui::GetIO().KeyShift);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openSelected();
    }

    // More menu at bottom-right
    float moreY = cardHeight - 44.0f;
    ImGui::SetCursorPos({width - 44.0f, moreY});
    if (ImGui::Button(ICON_MORE, {36.0f, 36.0f})) ImGui::OpenPopup("##card-actions");
    if (ImGui::BeginPopup("##card-actions")) {
        if (ImGui::MenuItem(ICON_OPEN "  打开", nullptr, false, replay.canOpen)) { select(replay.replayId, visibleIndex, false, false); openSelected(); }
        if (ImGui::MenuItem(ICON_SETTINGS "  编辑", nullptr, false, replay.canOpen)) { select(replay.replayId, visibleIndex, false, false); openSelected(); }
        if (ImGui::MenuItem(ICON_OPEN "  在文件夹中显示")) { auto const shown = screen::ReplayBrowser::showInFolder(replay); static_cast<void>(shown); }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_DELETE "  删除")) { select(replay.replayId, visibleIndex, false, false); mShowDeleteDialog = true; }
        ImGui::EndPopup();
    }

    // Info section
    float textY = kCardPadding + previewHeight + 18.0f;
    ImGui::SetCursorPos({kCardPadding, textY});
    ImGui::SetWindowFontScale(kFontScaleCardTitle);
    pushTextColor(kColorText);
    ImGui::TextUnformatted(replay.displayName().c_str());
    ImGui::PopStyleColor();

    ImGui::SetWindowFontScale(kFontScaleCardMeta);
    ImGui::SetCursorPos({kCardPadding, textY + 42.0f});
    ImGui::TextDisabled("%s", replay.worldName.empty() ? "未知世界" : replay.worldName.c_str());

    ImGui::SetCursorPos({kCardPadding, textY + 72.0f});
    ImGui::TextDisabled("%s  ·  %s", formatDuration(replay).c_str(), formatSize(replay.fileSize).c_str());

    if (!replay.canOpen) {
        ImGui::SetCursorPos({width - 44.0f, textY + 68.0f});
        ImGui::TextDisabled(ICON_WARNING);
        tooltip(replay.problem.c_str());
    }

    ImGui::SetWindowFontScale(kFontScaleBody);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PopID();
}

void ReplayBrowserWindow::drawGrid() {
    if (mVisible.empty()) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 60.0f);
        ImGui::SetWindowFontScale(kFontScaleHeader);
        ImGui::TextDisabled("没有回放文件");
        ImGui::SetWindowFontScale(kFontScaleBody);
        styleButton();
        if (ImGui::Button(ICON_EXPORT "  导入第一个回放", {220.0f, kControlHeight})) importReplay();
        popButtonStyle();
        return;
    }

    float available = ImGui::GetContentRegionAvail().x - kScreenMargin * 2.0f;
    int columns = 5;
    if (available < 5.0f * 240.0f + 4.0f * kCardGap) {
        columns = std::max(1, static_cast<int>((available + kCardGap) / (240.0f + kCardGap)));
    }
    float width = (available - (columns - 1) * kCardGap) / columns;

    // Header count
    ImGui::SetWindowFontScale(kFontScaleSmall);
    ImGui::SetCursorPosX(kScreenMargin);
    ImGui::TextDisabled("共 %zu 个回放文件", mVisible.size());
    ImGui::SetWindowFontScale(kFontScaleBody);
    ImGui::Dummy({0.0f, 16.0f});

    ImGui::SetCursorPosX(kScreenMargin);

    for (int item = 0; item < static_cast<int>(mVisible.size()); ++item) {
        if (item > 0 && item % columns != 0) ImGui::SameLine(0.0f, kCardGap);
        if (item % columns == 0 && item > 0) {
            ImGui::SetCursorPosX(kScreenMargin);
        }
        drawCard(mReplays[mVisible[static_cast<size_t>(item)]], static_cast<std::size_t>(item), width);
    }
}

void ReplayBrowserWindow::drawDetails() {
    ImVec2 const available = ImGui::GetContentRegionAvail();
    bool const stacked = available.x < 960.0f;
    float const gap = 20.0f;
    float const listWidth = stacked ? available.x : std::clamp(available.x * 0.34f, 360.0f, 480.0f);
    float const listHeight = stacked ? 260.0f : available.y;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorCardBg);
    ImGui::BeginChild("##details-list", {listWidth, listHeight}, false);
    ImGui::SetWindowFontScale(1.1f);
    ImGui::TextDisabled("回放文件  %zu", mVisible.size());
    ImGui::Separator();

    if (mVisible.empty()) {
        ImGui::TextDisabled("没有回放文件");
    } else {
        for (std::size_t visibleIndex = 0; visibleIndex < mVisible.size(); ++visibleIndex) {
            auto const& replay = mReplays[mVisible[visibleIndex]];
            bool const selected = mSelectedIds.contains(replay.replayId);
            ImGui::PushID(replay.replayId.c_str());
            ImGui::PushStyleColor(ImGuiCol_Header, kColorCardSelected);
            if (ImGui::Selectable("##detail-item", selected, ImGuiSelectableFlags_AllowDoubleClick, {0.0f, 92.0f})) {
                select(replay.replayId, visibleIndex, false, false);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openSelected();
            }
            ImGui::PopStyleColor();
            auto const min = ImGui::GetItemRectMin();
            auto const max = ImGui::GetItemRectMax();
            if (selected) ImGui::GetWindowDrawList()->AddRectFilled(min, {min.x + 4.0f, max.y}, kColorAccent);
            ImGui::GetWindowDrawList()->AddText({min.x + 16.0f, min.y + 12.0f}, kColorText, replay.displayName().c_str());
            ImGui::GetWindowDrawList()->AddText({min.x + 16.0f, min.y + 40.0f}, kColorTextDim,
                                                replay.worldName.empty() ? "未知世界" : replay.worldName.c_str());
            std::string const summary = formatDuration(replay) + "  ·  " + formatSize(replay.fileSize);
            ImGui::GetWindowDrawList()->AddText({min.x + 16.0f, min.y + 66.0f}, kColorTextDim, summary.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    if (stacked) {
        ImGui::Dummy({0.0f, gap});
    } else {
        ImGui::SameLine(0.0f, gap);
    }

    ImVec2 const panelSize = stacked ? ImVec2{available.x, std::max(420.0f, available.y - listHeight - gap)} : ImVec2{0.0f, available.y};
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorCardBg);
    ImGui::BeginChild("##details-panel", panelSize, false);

    auto replay = selectedReplay();
    if (!replay) {
        ImVec2 const textSize = ImGui::CalcTextSize("选择一个回放以查看详细信息");
        ImGui::SetCursorPos({(ImGui::GetWindowWidth() - textSize.x) * 0.5f, 60.0f});
        ImGui::TextDisabled("选择一个回放以查看详细信息");
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    float const margin = 24.0f;
    float const panelWidth = ImGui::GetWindowWidth() - margin * 2.0f;
    float const maxPreviewHeight = std::max(220.0f, ImGui::GetWindowHeight() * 0.53f);
    float previewWidth = panelWidth;
    float previewHeight = previewWidth * 9.0f / 16.0f;
    if (previewHeight > maxPreviewHeight) {
        previewHeight = maxPreviewHeight;
        previewWidth = previewHeight * 16.0f / 9.0f;
    }
    ImGui::SetCursorPos({margin + (panelWidth - previewWidth) * 0.5f, margin});
    drawPreview(**replay, {previewWidth, previewHeight});

    ImGui::SetCursorPos({margin, margin + previewHeight + 22.0f});
    ImGui::SetWindowFontScale(1.55f);
    pushTextColor(kColorText);
    ImGui::TextUnformatted((*replay)->displayName().c_str());
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.05f);
    ImGui::Separator();

    if (ImGui::BeginTable("##metadata", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        auto row = [](char const* label, std::string const& value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", label);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(value.c_str());
        };
        row("世界", (*replay)->worldName.empty() ? "未知" : (*replay)->worldName);
        row("时长", formatDuration(**replay));
        row("文件大小", formatSize((*replay)->fileSize));
        row("文件格式", ".playback");
        row("文件名", (*replay)->replayId);
        row("文件路径", (*replay)->path.string());
        ImGui::EndTable();
    }

    ImGui::SetWindowFontScale(kFontScaleBody);
    ImGui::Spacing();
    ImGui::BeginDisabled(!(*replay)->canOpen);
    styleButton();
    if (ImGui::Button("打开回放", {150.0f, kControlHeight})) openSelected();
    popButtonStyle();
    ImGui::EndDisabled();
    ImGui::SameLine();
    styleButton();
    if (ImGui::Button("复制路径", {140.0f, kControlHeight})) ImGui::SetClipboardText((*replay)->path.string().c_str());
    popButtonStyle();
    ImGui::SameLine();
    styleButton();
    if (ImGui::Button("所在文件夹", {160.0f, kControlHeight})) {
        auto const shown = screen::ReplayBrowser::showInFolder(**replay);
        static_cast<void>(shown);
    }
    popButtonStyle();
    ImGui::SameLine();
    styleButton();
    if (ImGui::Button("删除", {110.0f, kControlHeight})) mShowDeleteDialog = true;
    popButtonStyle();

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void ReplayBrowserWindow::drawActionBar() {
    auto replay = selectedReplay();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorCardBg);
    ImGui::BeginChild(
        "##actions",
        {0.0f, kActionBarHeight},
        false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    float const contentW = ImGui::GetWindowWidth();

    if (mSelectedIds.size() == 1 && replay) {
        ImGui::SetCursorPos({24.0f, 14.0f});
        ImGui::SetWindowFontScale(kFontScaleActionTitle);
        pushTextColor(kColorText);
        ImGui::TextUnformatted((*replay)->displayName().c_str());
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(kFontScaleActionMeta);
        ImGui::SetCursorPos({24.0f, 58.0f});
        ImGui::TextDisabled("%s  ·  %s", formatDuration(**replay).c_str(), formatSize((*replay)->fileSize).c_str());
    } else {
        ImGui::SetCursorPos({24.0f, 30.0f});
        ImGui::SetWindowFontScale(kFontScaleActionTitle);
        pushTextColor(kColorText);
        ImGui::Text("已选择 %zu 个文件", mSelectedIds.size());
        ImGui::PopStyleColor();
    }
    ImGui::SetWindowFontScale(kFontScaleBody);

    float const right = contentW - 24.0f;
    float const btnW  = 120.0f;
    ImGui::SetCursorPos({right - btnW * 3.0f - 24.0f, 20.0f});

    ImGui::BeginDisabled(!replay || !(*replay)->canOpen);
    styleButton();
    if (ImGui::Button("打开", {btnW, kControlHeight})) openSelected();
    popButtonStyle();
    ImGui::SameLine();
    styleButton();
    if (ImGui::Button("编辑", {btnW, kControlHeight})) openSelected();
    popButtonStyle();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, kColorDanger);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorDanger);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColorDanger);
    if (ImGui::Button("删除", {btnW, kControlHeight})) mShowDeleteDialog = true;
    ImGui::PopStyleColor(3);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void ReplayBrowserWindow::drawDeleteDialog() {
    if (mShowDeleteDialog) ImGui::OpenPopup("删除回放");
    if (ImGui::BeginPopupModal("删除回放", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("确定删除选中的回放文件吗");
        ImGui::TextDisabled("此操作无法撤销");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, kColorDanger);
        if (ImGui::Button(ICON_DELETE "  确认删除", {156.0f, kControlHeight})) {
            for (auto const& item : mReplays) {
                if (!mSelectedIds.contains(item.replayId)) continue;
                auto const deleted = screen::ReplayBrowser::deleteReplay(item, mOperationError);
                static_cast<void>(deleted);
                if (!mOperationError.empty()) break;
            }
            mShowDeleteDialog = false;
            ImGui::CloseCurrentPopup();
            refresh();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button(ICON_CLOSE "  取消", {120.0f, kControlHeight})) { mShowDeleteDialog = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (!mOperationError.empty()) {
        ImGui::OpenPopup("回放操作失败");
        if (ImGui::BeginPopupModal("回放操作失败", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", mOperationError.c_str());
            if (ImGui::Button(ICON_CHECK "  确定", {120.0f, kControlHeight})) { mOperationError.clear(); ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
}

} // namespace playback::refactor::replay_browser
