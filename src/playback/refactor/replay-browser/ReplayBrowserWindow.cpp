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
// 统一字号：基础字体 14px，全 UI 只允许 18 / 24 / 30 三档。
constexpr float kFontScaleSmall = 18.0f / 14.0f; // 18px
constexpr float kFontScaleBody  = 24.0f / 14.0f; // 24px
constexpr float kFontScaleLarge = 30.0f / 14.0f; // 30px

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

std::string formatModifiedTime(std::filesystem::file_time_type const& time) {
    if (time == std::filesystem::file_time_type{}) return "未知";
    auto const sysTime =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(time - std::filesystem::file_time_type::clock::now()
                                                                          + std::chrono::system_clock::now());
    std::time_t const t = std::chrono::system_clock::to_time_t(sysTime);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::array<char, 64> buf{};
    std::strftime(buf.data(), buf.size(), "%Y-%m-%d %H:%M", &tm);
    return buf.data();
}

// 按钮自适应宽度：文字（含图标）+ 左右留白。
float autoWidth(std::string const& text) { return ImGui::CalcTextSize(text.c_str()).x + 34.0f; }

char const* sortLabel(screen::ReplaySort sort) {
    switch (sort) {
    case screen::ReplaySort::ReplayName: return "名称";
    case screen::ReplaySort::WorldName: return "世界";
    case screen::ReplaySort::Duration: return "时长";
    case screen::ReplaySort::FileSize: return "大小";
    default: return "日期";
    }
}

char const* filterLabel(ReplayFilter filter) {
    switch (filter) {
    case ReplayFilter::Playable: return "仅正常";
    case ReplayFilter::Broken: return "仅异常";
    default: return "全部";
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

// 返回按钮：默认透明，悬停才显示浅灰色。
bool backButton(char const* icon, float size) {
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorButtonHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColorButtonActive);
    bool clicked = ImGui::Button(icon, {size, size});
    ImGui::PopStyleColor(3);
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
        auto const& replay = mReplays[index];
        bool pass = replay.matches(mSearch);
        if (pass && mFilter == ReplayFilter::Playable) pass = replay.canOpen;
        if (pass && mFilter == ReplayFilter::Broken) pass = !replay.canOpen;
        if (pass) mVisible.push_back(index);
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
    drawRenameDialog();

    ImGui::SetWindowFontScale(kFontScaleBody);
    ImGui::End();
    ImGui::PopStyleColor();
}

void ReplayBrowserWindow::drawNavigation() {
    ImGui::BeginChild("##header", {0.0f, kNavHeight}, false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    float const width  = ImGui::GetWindowWidth();
    float const margin = 24.0f;
    float const gap    = 12.0f;
    float const iconW  = kControlHeight;
    float const searchW = 300.0f;

    // 文字按钮宽度自适应：按当前标签（含图标）计算，避免文字被裁剪。
    std::string const importLabelText = std::string(ICON_EXPORT) + "  导入";
    std::string const filterLabelText = std::string(ICON_FILTER) + "  过滤  " + filterLabel(mFilter);
    std::string const sortLabelText   = std::string(ICON_SORT) + "  排序  " + sortLabel(mSort) + (mDescending ? " ↓" : " ↑");
    std::string const viewLabelText   = mViewMode == ViewMode::Grid ? std::string(ICON_GRID) + "  平铺" : std::string(ICON_LIST) + "  列表";
    float const importW = autoWidth(importLabelText);
    float const filterW = autoWidth(filterLabelText);
    float const sortW   = autoWidth(sortLabelText);
    float const viewW   = autoWidth(viewLabelText);
    float const ctrlW   = searchW + importW + filterW + sortW + viewW + iconW + gap * 6.0f;
    float const startX  = std::max(margin + 200.0f, width - margin - ctrlW);
    float const y       = (kNavHeight - kControlHeight) * 0.5f;

    // 返回按钮：← 图标，位于标题左侧，默认透明、悬停浅灰。
    ImGui::SetCursorPos({margin, y});
    if (backButton(ICON_BACK, iconW)) close();
    tooltip("返回主菜单");

    // 标题：30px
    ImGui::SetCursorPos({margin + iconW + gap, (kNavHeight - 30.0f * 1.4f) * 0.5f});
    ImGui::SetWindowFontScale(kFontScaleLarge);
    pushTextColor(kColorText);
    ImGui::TextUnformatted("回放列表");
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(kFontScaleBody);

    float x = startX;

    // 搜索框：修正为深灰底、白字，与按钮风格一致。
    ImGui::SetCursorPos({x, y});
    ImGui::SetNextItemWidth(searchW);
    std::array<char, 256> search{};
    std::copy_n(mSearch.data(), std::min(mSearch.size(), search.size() - 1), search.data());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12.0f, (kControlHeight - 24.0f) * 0.5f});
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kColorButton);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kColorButtonHover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kColorButtonActive);
    ImGui::PushStyleColor(ImGuiCol_Text, kColorText);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, kColorTextDim);
    if (ImGui::InputTextWithHint("##search", ICON_SEARCH "  搜索回放文件", search.data(), search.size())) {
        mSearch = search.data();
        rebuildVisible();
    }
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar();
    x += searchW + gap;

    // 导入
    ImGui::SetCursorPos({x, y});
    styleButton();
    if (ImGui::Button(importLabelText.c_str(), {importW, kControlHeight})) importReplay();
    popButtonStyle();
    tooltip("导入回放文件");
    x += importW + gap;

    // 过滤下拉框：图标在左，宽度随文字自适应，无下拉箭头。
    ImGui::SetCursorPos({x, y});
    styleButton(mFilter != ReplayFilter::All);
    if (ImGui::Button(filterLabelText.c_str(), {filterW, kControlHeight})) {
        ImGui::OpenPopup("##filter-menu");
    }
    popButtonStyle();
    if (ImGui::BeginPopup("##filter-menu")) {
        ImGui::SetWindowFontScale(kFontScaleBody);
        for (auto filter : {ReplayFilter::All, ReplayFilter::Playable, ReplayFilter::Broken}) {
            if (ImGui::MenuItem(filterLabel(filter), nullptr, mFilter == filter)) {
                mFilter = filter;
                rebuildVisible();
            }
        }
        ImGui::EndPopup();
    }
    x += filterW + gap;

    // 排序下拉框：图标在左，宽度随文字自适应，无下拉箭头。
    ImGui::SetCursorPos({x, y});
    styleButton();
    if (ImGui::Button(sortLabelText.c_str(), {sortW, kControlHeight})) {
        ImGui::OpenPopup("##sort-menu");
    }
    popButtonStyle();
    if (ImGui::BeginPopup("##sort-menu")) {
        ImGui::SetWindowFontScale(kFontScaleBody);
        for (auto sort : {screen::ReplaySort::LastModified, screen::ReplaySort::ReplayName,
                          screen::ReplaySort::WorldName, screen::ReplaySort::Duration, screen::ReplaySort::FileSize}) {
            if (ImGui::MenuItem(sortLabel(sort), nullptr, mSort == sort)) {
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
        ImGui::EndPopup();
    }
    x += sortW + gap;

    // 视图切换：单个按钮，点击在平铺/列表之间切换。
    ImGui::SetCursorPos({x, y});
    if (textButton(viewLabelText.c_str(), viewW, true)) {
        mViewMode = mViewMode == ViewMode::Grid ? ViewMode::Details : ViewMode::Grid;
    }
    tooltip(mViewMode == ViewMode::Grid ? "切换到列表视图" : "切换到平铺视图");
    x += viewW + gap;

    // 设置
    ImGui::SetCursorPos({x, y});
    if (iconButton(ICON_SETTINGS, iconW)) ImGui::OpenPopup("##settings");
    tooltip("设置");
    if (ImGui::BeginPopup("##settings")) {
        ImGui::SetWindowFontScale(kFontScaleBody);
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
    // 注意：InvisibleButton 默认在“释放”时返回 true，而 IsMouseDoubleClicked 只在第二次“按下”
    // 那一帧为 true，二者不同帧，因此不能把双击判定放进按钮返回值分支里。改为在按下帧结合悬停判定。
    ImGui::SetCursorPos({0.0f, 0.0f});
    ImGui::InvisibleButton("##select-card", {width, cardHeight});
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && ImGui::IsItemHovered()) {
        select(replay.replayId, visibleIndex, false, false);
        openSelected();
    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        select(replay.replayId, visibleIndex, ImGui::GetIO().KeyCtrl, ImGui::GetIO().KeyShift);
    }

    // 右下角详细信息图标：透明背景，悬停时图标本身变白并显示详细信息。
    float const infoSize = 36.0f;
    ImGui::SetCursorPos({width - 44.0f, cardHeight - 44.0f});
    ImGui::InvisibleButton("##card-info", {infoSize, infoSize});
    bool const infoHovered = ImGui::IsItemHovered();
    auto const infoMin     = ImGui::GetItemRectMin();
    auto const infoText    = ImGui::CalcTextSize(ICON_INFO);
    ImGui::GetWindowDrawList()->AddText(
        ImGui::GetFont(),
        ImGui::GetFontSize(),
        {infoMin.x + (infoSize - infoText.x) * 0.5f, infoMin.y + (infoSize - infoText.y) * 0.5f},
        infoHovered ? kColorText : kColorTextDim,
        ICON_INFO
    );
    if (infoHovered) {
        ImGui::BeginTooltip();
        ImGui::SetWindowFontScale(kFontScaleSmall);
        auto detailRow = [](char const* label, std::string const& value) {
            ImGui::TextDisabled("%s", label);
            ImGui::SameLine();
            ImGui::TextUnformatted(value.c_str());
        };
        detailRow("名称", replay.displayName());
        detailRow("世界", replay.worldName.empty() ? "未知" : replay.worldName);
        detailRow("时长", formatDuration(replay));
        detailRow("大小", formatSize(replay.fileSize));
        detailRow("修改时间", formatModifiedTime(replay.lastModified));
        detailRow("文件名", replay.replayId);
        ImGui::Spacing();
        ImGui::TextWrapped("路径: %s", replay.path.string().c_str());
        if (!replay.canOpen) {
            ImGui::Spacing();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kColorDanger), "异常: %s", replay.problem.c_str());
        }
        ImGui::EndTooltip();
    }

    // Info section：标题 30px，元信息 18px
    float textY = kCardPadding + previewHeight + 16.0f;
    ImGui::SetCursorPos({kCardPadding, textY});
    ImGui::SetWindowFontScale(kFontScaleLarge);
    pushTextColor(kColorText);
    ImGui::TextUnformatted(replay.displayName().c_str());
    ImGui::PopStyleColor();

    ImGui::SetWindowFontScale(kFontScaleSmall);
    ImGui::SetCursorPos({kCardPadding, textY + 46.0f});
    ImGui::TextDisabled("%s", replay.worldName.empty() ? "未知世界" : replay.worldName.c_str());

    ImGui::SetCursorPos({kCardPadding, textY + 76.0f});
    ImGui::TextDisabled("%s  ·  %s", formatDuration(replay).c_str(), formatSize(replay.fileSize).c_str());

    if (!replay.canOpen) {
        ImGui::SetCursorPos({width - 44.0f, textY + 72.0f});
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
        ImGui::SetWindowFontScale(kFontScaleLarge);
        ImGui::TextDisabled("没有回放文件");
        ImGui::SetWindowFontScale(kFontScaleBody);
        styleButton();
        if (ImGui::Button(ICON_EXPORT "  导入第一个回放", {240.0f, kControlHeight})) importReplay();
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
    ImGui::SetWindowFontScale(kFontScaleSmall);
    ImGui::TextDisabled("回放文件  %zu", mVisible.size());
    ImGui::Separator();

    if (mVisible.empty()) {
        ImGui::TextDisabled("没有回放文件");
    } else {
        auto* const font = ImGui::GetFont();
        for (std::size_t visibleIndex = 0; visibleIndex < mVisible.size(); ++visibleIndex) {
            auto const& replay = mReplays[mVisible[visibleIndex]];
            bool const selected = mSelectedIds.contains(replay.replayId);
            ImGui::PushID(replay.replayId.c_str());
            ImGui::PushStyleColor(ImGuiCol_Header, kColorCardSelected);
            ImGui::Selectable("##detail-item", selected, ImGuiSelectableFlags_AllowDoubleClick, {0.0f, 92.0f});
            ImGui::PopStyleColor();
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && ImGui::IsItemHovered()) {
                select(replay.replayId, visibleIndex, false, false);
                openSelected();
            } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                select(replay.replayId, visibleIndex, false, false);
            }
            auto const min = ImGui::GetItemRectMin();
            auto const max = ImGui::GetItemRectMax();
            if (selected) ImGui::GetWindowDrawList()->AddRectFilled(min, {min.x + 4.0f, max.y}, kColorAccent);
            std::string const summary = formatDuration(replay) + "  ·  " + formatSize(replay.fileSize);
            ImGui::GetWindowDrawList()->AddText(font, 24.0f, {min.x + 16.0f, min.y + 8.0f}, kColorText, replay.displayName().c_str());
            ImGui::GetWindowDrawList()->AddText(font, 18.0f, {min.x + 16.0f, min.y + 44.0f}, kColorTextDim,
                                                replay.worldName.empty() ? "未知世界" : replay.worldName.c_str());
            ImGui::GetWindowDrawList()->AddText(font, 18.0f, {min.x + 16.0f, min.y + 68.0f}, kColorTextDim, summary.c_str());
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
    ImGui::SetWindowFontScale(kFontScaleLarge);
    pushTextColor(kColorText);
    ImGui::TextUnformatted((*replay)->displayName().c_str());
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(kFontScaleSmall);
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
        ImGui::SetCursorPos({24.0f, 10.0f});
        ImGui::SetWindowFontScale(kFontScaleLarge);
        pushTextColor(kColorText);
        ImGui::TextUnformatted((*replay)->displayName().c_str());
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(kFontScaleSmall);
        ImGui::SetCursorPos({24.0f, 62.0f});
        ImGui::TextDisabled("%s  ·  %s", formatDuration(**replay).c_str(), formatSize((*replay)->fileSize).c_str());
    } else {
        ImGui::SetCursorPos({24.0f, 28.0f});
        ImGui::SetWindowFontScale(kFontScaleLarge);
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
    if (ImGui::Button("编辑", {btnW, kControlHeight})) openRenameDialog();
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

void ReplayBrowserWindow::openRenameDialog() {
    auto replay = selectedReplay();
    if (!replay || !(*replay)->canOpen) return;
    mRenameBuffer  = (*replay)->displayName();
    mRenameDialogOpen = true;
}

void ReplayBrowserWindow::drawRenameDialog() {
    auto replay = selectedReplay();
    if (mRenameDialogOpen) {
        ImGui::OpenPopup("编辑回放名称");
        mRenameDialogOpen = false;
    }
    if (!ImGui::BeginPopupModal("编辑回放名称", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::SetWindowFontScale(kFontScaleBody);
    ImGui::TextDisabled("修改名称会同时更新回放元数据与文件名");
    ImGui::Spacing();

    std::array<char, 256> buffer{};
    std::copy_n(mRenameBuffer.data(), std::min(mRenameBuffer.size(), buffer.size() - 1), buffer.data());
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kColorButton);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kColorButtonHover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kColorButtonActive);
    ImGui::PushStyleColor(ImGuiCol_Text, kColorText);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12.0f, 10.0f});
    ImGui::SetNextItemWidth(460.0f);
    bool const submitted = ImGui::InputText(
        "##rename-input",
        buffer.data(),
        buffer.size(),
        ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue
    );
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    if (submitted) mRenameBuffer = buffer.data();

    ImGui::Spacing();
    bool const empty = mRenameBuffer.empty();
    ImGui::BeginDisabled(empty);
    styleButton();
    bool const saved = ImGui::Button(ICON_CHECK "  保存", {140.0f, kControlHeight});
    popButtonStyle();
    ImGui::EndDisabled();
    ImGui::SameLine();
    styleButton();
    bool const cancelled = ImGui::Button(ICON_CLOSE "  取消", {120.0f, kControlHeight});
    popButtonStyle();

    bool const confirm = saved || (submitted && !empty);
    if (confirm && replay) {
        if (screen::ReplayBrowser::renameReplay(**replay, mRenameBuffer, mOperationError)) {
            ImGui::CloseCurrentPopup();
            refresh();
        } else {
            ImGui::CloseCurrentPopup();
        }
    } else if (cancelled) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace playback::refactor::replay_browser
