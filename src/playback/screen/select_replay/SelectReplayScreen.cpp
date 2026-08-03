#include "SelectReplayScreen.h"

#include "playback/editor/renderer/ImGuiRenderer.h"
#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/iconfont.h"

#include "ll/api/i18n/I18n.h"
#include "ll/api/utils/StringUtils.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <windows.h>

#include <commdlg.h>

namespace playback::screen::select_replay {

using namespace ll::i18n_literals;

namespace {

constexpr float kNavHeight       = 84.0f;
constexpr float kActionBarHeight = 104.0f;
constexpr float kScreenMargin    = 50.0f;
constexpr float kCardGap         = 24.0f;
constexpr float kCardPadding     = 16.0f;
constexpr float kControlHeight   = 52.0f;
// 统一字号：基础字体 14px，全 UI 只允许 18 / 24 / 30 三档。
constexpr float kFontScaleSmall = 18.0f / 14.0f; // 18px
constexpr float kFontScaleBody  = 24.0f / 14.0f; // 24px
constexpr float kFontScaleLarge = 30.0f / 14.0f; // 30px

constexpr ImU32 kColorAccent       = IM_COL32(58, 140, 240, 255);
constexpr ImU32 kColorAccentDim    = IM_COL32(58, 140, 240, 130);
constexpr ImU32 kColorBg           = IM_COL32(28, 28, 28, 255);
constexpr ImU32 kColorCardBg       = IM_COL32(48, 48, 48, 255);
constexpr ImU32 kColorCardSelected = IM_COL32(85, 85, 85, 255);
constexpr ImU32 kColorButton       = IM_COL32(55, 55, 55, 255);
constexpr ImU32 kColorButtonHover  = IM_COL32(70, 70, 70, 255);
constexpr ImU32 kColorButtonActive = IM_COL32(90, 90, 90, 255);
constexpr ImU32 kColorPreviewBg    = IM_COL32(30, 42, 58, 255);
constexpr ImU32 kColorDanger       = IM_COL32(210, 60, 60, 255);
constexpr ImU32 kColorText         = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kColorTextDim      = IM_COL32(180, 180, 180, 255);

std::string formatSize(std::uintmax_t bytes) {
    constexpr std::array<char const*, 4> units{"B", "KB", "MB", "GB"};
    double                               size = static_cast<double>(bytes);
    size_t                               unit = 0;
    while (size >= 1024.0 && unit + 1 < units.size()) {
        size /= 1024.0;
        ++unit;
    }
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1) << size << ' ' << units[unit];
    return stream.str();
}

std::string formatDuration(playback::editor::ReplayBrowserEntry const& replay) {
    int seconds = (replay.totalTicks > 0 ? replay.totalTicks : replay.durationTicks) / 20;
    return "playback.replayBrowser.duration"_tr(seconds / 60, seconds % 60);
}

std::string formatModifiedTime(std::filesystem::file_time_type const& time) {
    if (time == std::filesystem::file_time_type{}) return "playback.replayBrowser.unknown"_tr();
    auto const sysTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
    );
    std::time_t const t = std::chrono::system_clock::to_time_t(sysTime);
    std::tm           tm{};
    localtime_s(&tm, &t);
    std::array<char, 64> buf{};
    std::strftime(buf.data(), buf.size(), "%Y-%m-%d %H:%M", &tm);
    return buf.data();
}

// 按钮自适应宽度：文字（含图标）+ 左右留白。
float autoWidth(std::string const& text) { return ImGui::CalcTextSize(text.c_str()).x + 34.0f; }

std::string sortLabel(BrowserSort sort) {
    switch (sort) {
    case BrowserSort::ReplayName:
        return "playback.replayBrowser.sort.name"_tr();
    case BrowserSort::WorldName:
        return "playback.replayBrowser.sort.world"_tr();
    case BrowserSort::Duration:
        return "playback.replayBrowser.sort.duration"_tr();
    case BrowserSort::FileSize:
        return "playback.replayBrowser.sort.size"_tr();
    default:
        return "playback.replayBrowser.sort.date"_tr();
    }
}

int compareText(std::string_view left, std::string_view right) {
    auto lower = [](std::string_view value) {
        std::string result(value);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return result;
    };
    auto const normalizedLeft  = lower(left);
    auto const normalizedRight = lower(right);
    if (normalizedLeft < normalizedRight) return -1;
    if (normalizedLeft > normalizedRight) return 1;
    return 0;
}

std::string filterLabel(ReplayFilter filter) {
    switch (filter) {
    case ReplayFilter::Playable:
        return "playback.replayBrowser.filter.playable"_tr();
    case ReplayFilter::Broken:
        return "playback.replayBrowser.filter.broken"_tr();
    default:
        return "playback.replayBrowser.filter.all"_tr();
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

SelectReplayScreen& SelectReplayScreen::getInstance() {
    static SelectReplayScreen instance;
    return instance;
}

std::vector<playback::editor::ReplayBrowserEntry> const& SelectReplayScreen::replays() const {
    static std::vector<playback::editor::ReplayBrowserEntry> const empty;
    return mState && mState->snapshot ? mState->snapshot->replays : empty;
}

void SelectReplayScreen::submit(playback::editor::EditorAction action) const {
    if (mSubmit) (*mSubmit)(std::move(action));
}

void SelectReplayScreen::syncSnapshot() {
    auto const revision = mState && mState->snapshot ? mState->snapshot->revision : 0;
    if (revision == mSnapshotRevision) return;
    mSnapshotRevision = revision;
    mSelectedIds.clear();
    mSelectionAnchor.reset();
    mShowDeleteDialog = false;
    rebuildVisible();
}

void SelectReplayScreen::rebuildVisible() {
    auto const& items = replays();
    mVisible.clear();
    mVisible.reserve(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
        auto const& replay = items[index];
        bool        pass   = replay.matches(mSearch);
        if (pass && mFilter == ReplayFilter::Playable) pass = replay.canOpen;
        if (pass && mFilter == ReplayFilter::Broken) pass = !replay.canOpen;
        if (pass) mVisible.push_back(index);
    }

    auto less = [&](std::size_t leftIndex, std::size_t rightIndex) {
        auto const& left  = items[leftIndex];
        auto const& right = items[rightIndex];
        int         result{};
        switch (mSort) {
        case BrowserSort::ReplayName:
            result = compareText(left.displayName(), right.displayName());
            break;
        case BrowserSort::WorldName:
            result = compareText(left.worldName, right.worldName);
            break;
        case BrowserSort::Duration: {
            auto const leftTicks  = left.totalTicks > 0 ? left.totalTicks : left.durationTicks;
            auto const rightTicks = right.totalTicks > 0 ? right.totalTicks : right.durationTicks;
            result                = leftTicks < rightTicks ? -1 : leftTicks > rightTicks ? 1 : 0;
            break;
        }
        case BrowserSort::FileSize:
            result = left.fileSize < right.fileSize ? -1 : left.fileSize > right.fileSize ? 1 : 0;
            break;
        case BrowserSort::LastModified:
        default:
            result = left.lastModified < right.lastModified ? -1 : left.lastModified > right.lastModified ? 1 : 0;
            break;
        }
        if (result == 0) result = compareText(left.replayId, right.replayId);
        return mDescending ? result > 0 : result < 0;
    };
    std::stable_sort(mVisible.begin(), mVisible.end(), less);
}

void SelectReplayScreen::select(std::string_view replayId, std::size_t visibleIndex, bool toggle, bool range) {
    if (range && mSelectionAnchor && *mSelectionAnchor < mVisible.size()) {
        auto const first = std::min(*mSelectionAnchor, visibleIndex);
        auto const last  = std::max(*mSelectionAnchor, visibleIndex);
        for (auto index = first; index <= last; ++index) mSelectedIds.insert(replays()[mVisible[index]].replayId);
    } else if (toggle) {
        if (!mSelectedIds.erase(std::string(replayId))) mSelectedIds.insert(std::string(replayId));
        mSelectionAnchor = visibleIndex;
    } else {
        mSelectedIds     = {std::string(replayId)};
        mSelectionAnchor = visibleIndex;
    }
}

std::optional<playback::editor::ReplayBrowserEntry const*> SelectReplayScreen::selectedReplay() const {
    if (mSelectedIds.size() != 1) return std::nullopt;
    auto const& items = replays();
    auto        it    = std::find_if(items.begin(), items.end(), [&](auto const& replay) {
        return mSelectedIds.contains(replay.replayId);
    });
    return it == items.end() ? std::nullopt : std::optional<playback::editor::ReplayBrowserEntry const*>{&*it};
}

void SelectReplayScreen::openSelected() {
    auto replay = selectedReplay();
    if (!replay || !(*replay)->canOpen) return;
    playback::editor::EditorAction action{playback::editor::EditorActionType::OpenReplay};
    action.path     = (*replay)->path;
    action.replayId = (*replay)->replayId;
    submit(std::move(action));
}

void SelectReplayScreen::importReplay() {
    std::array<wchar_t, 32768> file{};
    std::wstring               filter =
        ll::string_utils::str2wstr("playback.replayBrowser.openDialog.replayFiles"_tr()) + L" (*.playback;*.zip)";
    filter.push_back(L'\0');
    filter += L"*.playback;*.zip";
    filter.push_back(L'\0');
    filter.push_back(L'\0');

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile   = file.data();
    dialog.nMaxFile    = static_cast<DWORD>(file.size());
    dialog.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return;
    playback::editor::EditorAction action{playback::editor::EditorActionType::ImportReplay};
    action.path = file.data();
    submit(std::move(action));
}

void SelectReplayScreen::draw(playback::editor::ReplayBrowserState const& state, SubmitAction const& submitAction) {
    if (!state.visible) return;
    mState  = &state;
    mSubmit = &submitAction;
    syncSnapshot();
    playback::editor::ui::EditorTheme theme;
    theme.apply();
    auto const& io = ImGui::GetIO();

    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kColorBg);
    ImGui::Begin(
        "##replay-browser",
        nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoSavedSettings
    );

    ImGui::SetWindowFontScale(kFontScaleBody);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        submit({playback::editor::EditorActionType::CloseReplayBrowser});
    }

    ImGui::BeginDisabled(state.busy());
    drawNavigation();
    ImGui::Separator();

    float const actionHeight = (mViewMode == ViewMode::Grid && !mSelectedIds.empty()) ? kActionBarHeight : 0.0f;
    ImGui::BeginChild("##content", {0.0f, -actionHeight}, false);
    if (mViewMode == ViewMode::Grid) drawGrid();
    else drawDetails();
    ImGui::EndChild();

    if (mViewMode == ViewMode::Grid && !mSelectedIds.empty()) drawActionBar();
    ImGui::EndDisabled();

    drawDeleteDialog();
    drawRenameDialog();

    ImGui::SetWindowFontScale(kFontScaleBody);
    ImGui::End();
    ImGui::PopStyleColor();
    mState  = nullptr;
    mSubmit = nullptr;
}

void SelectReplayScreen::drawNavigation() {
    ImGui::BeginChild(
        "##header",
        {0.0f, kNavHeight},
        false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    float const width   = ImGui::GetWindowWidth();
    float const margin  = 24.0f;
    float const gap     = 12.0f;
    float const iconW   = kControlHeight;
    float const searchW = 300.0f;

    // 文字按钮宽度自适应：按当前标签（含图标）计算，避免文字被裁剪。
    std::string const importLabelText =
        std::string(ICON_EXPORT) + "  " + "playback.replayBrowser.navigation.import"_tr();
    std::string const filterLabelText =
        std::string(ICON_FILTER) + "  " + "playback.replayBrowser.navigation.filter"_tr() + "  " + filterLabel(mFilter);
    std::string const sortLabelText = std::string(ICON_SORT) + "  " + "playback.replayBrowser.navigation.sort"_tr()
                                    + "  " + sortLabel(mSort) + (mDescending ? " ↓" : " ↑");
    std::string const viewLabelText = mViewMode == ViewMode::Grid
                                        ? std::string(ICON_GRID) + "  " + "playback.replayBrowser.navigation.grid"_tr()
                                        : std::string(ICON_LIST) + "  " + "playback.replayBrowser.navigation.list"_tr();
    float const       importW       = autoWidth(importLabelText);
    float const       filterW       = autoWidth(filterLabelText);
    float const       sortW         = autoWidth(sortLabelText);
    float const       viewW         = autoWidth(viewLabelText);
    float const       ctrlW         = searchW + importW + filterW + sortW + viewW + iconW * 2.0f + gap * 7.0f;
    float const       startX        = std::max(margin + 200.0f, width - margin - ctrlW);
    float const       y             = (kNavHeight - kControlHeight) * 0.5f;

    // 返回按钮：← 图标，位于标题左侧，默认透明、悬停浅灰。
    ImGui::SetCursorPos({margin, y});
    if (backButton(ICON_BACK, iconW)) submit({playback::editor::EditorActionType::CloseReplayBrowser});
    tooltip("playback.replayBrowser.navigation.back"_tr().c_str());

    // 标题：30px
    ImGui::SetCursorPos({margin + iconW + gap, (kNavHeight - 30.0f * 1.4f) * 0.5f});
    ImGui::SetWindowFontScale(kFontScaleLarge);
    pushTextColor(kColorText);
    ImGui::TextUnformatted("playback.replayBrowser.title"_tr().c_str());
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
    std::string const searchHint = std::string(ICON_SEARCH) + "  " + "playback.replayBrowser.navigation.search"_tr();
    if (ImGui::InputTextWithHint("##search", searchHint.c_str(), search.data(), search.size())) {
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
    tooltip("playback.replayBrowser.navigation.importTooltip"_tr().c_str());
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
            if (ImGui::MenuItem(filterLabel(filter).c_str(), nullptr, mFilter == filter)) {
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
        for (auto sort :
             {BrowserSort::LastModified,
              BrowserSort::ReplayName,
              BrowserSort::WorldName,
              BrowserSort::Duration,
              BrowserSort::FileSize}) {
            if (ImGui::MenuItem(sortLabel(sort).c_str(), nullptr, mSort == sort)) {
                mSort = sort;
                rebuildVisible();
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("playback.replayBrowser.sort.ascending"_tr().c_str(), nullptr, !mDescending)) {
            mDescending = false;
            rebuildVisible();
        }
        if (ImGui::MenuItem("playback.replayBrowser.sort.descending"_tr().c_str(), nullptr, mDescending)) {
            mDescending = true;
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
    std::string const viewTooltip = mViewMode == ViewMode::Grid ? "playback.replayBrowser.navigation.switchToList"_tr()
                                                                : "playback.replayBrowser.navigation.switchToGrid"_tr();
    tooltip(viewTooltip.c_str());
    x += viewW + gap;

    ImGui::SetCursorPos({x, y});
    if (iconButton(ICON_REFRESH, iconW)) {
        submit({playback::editor::EditorActionType::RefreshReplayBrowser});
    }
    tooltip("playback.replayBrowser.navigation.refresh"_tr().c_str());
    x += iconW + gap;

    // 设置
    ImGui::SetCursorPos({x, y});
    if (iconButton(ICON_SETTINGS, iconW)) ImGui::OpenPopup("##settings");
    tooltip("playback.replayBrowser.navigation.settings"_tr().c_str());
    if (ImGui::BeginPopup("##settings")) {
        ImGui::SetWindowFontScale(kFontScaleBody);
        ImGui::TextDisabled("%s", "playback.replayBrowser.navigation.settingsUnavailable"_tr().c_str());
        ImGui::EndPopup();
    }

    ImGui::EndChild();
}

void SelectReplayScreen::drawPreview(playback::editor::ReplayBrowserEntry const& replay, ImVec2 size) {
    auto start = ImGui::GetCursorScreenPos();
    auto end   = ImVec2(start.x + size.x, start.y + size.y);
    ImGui::GetWindowDrawList()->AddRectFilled(start, end, kColorPreviewBg, 0.0f);

    auto texture = playback::editor::renderer::gImGuiRenderer.acquireReplayThumbnailTexture(
        replay.path.string(),
        replay.thumbnailPng
    );
    if (texture) {
        // 缩略图源固定为 16:9，按目标区域比例居中裁剪，绝不拉伸。
        constexpr float sourceAspect = 16.0f / 9.0f;
        float const     targetAspect = size.x / size.y;
        ImVec2          uv0{0.0f, 0.0f};
        ImVec2          uv1{1.0f, 1.0f};
        if (targetAspect < sourceAspect) {
            float const visibleWidth = targetAspect / sourceAspect;
            uv0.x                    = (1.0f - visibleWidth) * 0.5f;
            uv1.x                    = 1.0f - uv0.x;
        } else if (targetAspect > sourceAspect) {
            float const visibleHeight = sourceAspect / targetAspect;
            uv0.y                     = (1.0f - visibleHeight) * 0.5f;
            uv1.y                     = 1.0f - uv0.y;
        }
        ImGui::Image(texture, size, uv0, uv1);
    } else {
        auto              center = ImVec2(start.x + size.x * 0.5f, start.y + size.y * 0.5f);
        std::string const msg    = "playback.replayBrowser.previewUnavailable"_tr();
        auto              ts     = ImGui::CalcTextSize(msg.c_str());
        ImGui::GetWindowDrawList()
            ->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), kColorTextDim, msg.c_str());
        ImGui::Dummy(size);
    }
}

void SelectReplayScreen::drawCard(
    playback::editor::ReplayBrowserEntry const& replay,
    std::size_t                                 visibleIndex,
    float                                       width
) {
    bool const  selected   = mSelectedIds.contains(replay.replayId);
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
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
        && ImGui::IsItemHovered()) {
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

    // tooltip 淡入：悬停瞬间透明度为 0，0.5s 后开始渐显，0.8s 时恢复为 1，
    // 规避首次悬停时 tooltip 短暂渲染异常。
    // 注意：悬停起始时间不能用成员变量保存——多张卡片共享同一成员，悬停卡片之后
    // 绘制的其它卡片每帧会把状态重置为 -1，导致 elapsed 恒为 0、tooltip 恒透明。
    // 改用 ImGuiStorage 按 per-card ID（PushID 已隔离卡片）保存，互不干扰。
    double const        now         = ImGui::GetTime();
    ImGuiStorage* const fadeStorage = ImGui::GetStateStorage();
    ImGuiID const       fadeKey     = ImGui::GetID("##info-fade");
    if (infoHovered) {
        if (fadeStorage->GetFloat(fadeKey, -1.0f) < 0.0f) fadeStorage->SetFloat(fadeKey, static_cast<float>(now));
    } else {
        fadeStorage->SetFloat(fadeKey, -1.0f);
    }
    float tooltipAlpha = 1.0f;
    if (infoHovered) {
        double const elapsed = now - fadeStorage->GetFloat(fadeKey, -1.0f);
        tooltipAlpha         = elapsed <= 0.5 ? 0.0f : static_cast<float>(std::clamp((elapsed - 0.5) / 0.3, 0.0, 1.0));
    }

    auto const infoMin  = ImGui::GetItemRectMin();
    auto const infoText = ImGui::CalcTextSize(ICON_INFO);
    ImGui::GetWindowDrawList()->AddText(
        ImGui::GetFont(),
        ImGui::GetFontSize(),
        {infoMin.x + (infoSize - infoText.x) * 0.5f, infoMin.y + (infoSize - infoText.y) * 0.5f},
        infoHovered ? kColorText : kColorTextDim,
        ICON_INFO
    );
    if (infoHovered) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, tooltipAlpha);
        ImGui::BeginTooltip();
        ImGui::SetWindowFontScale(kFontScaleSmall);
        auto detailRow = [](char const* label, std::string const& value) {
            ImGui::TextDisabled("%s", label);
            ImGui::SameLine();
            ImGui::TextUnformatted(value.c_str());
        };
        detailRow("playback.replayBrowser.field.name"_tr().c_str(), replay.displayName());
        detailRow(
            "playback.replayBrowser.field.world"_tr().c_str(),
            replay.worldName.empty() ? "playback.replayBrowser.unknown"_tr() : replay.worldName
        );
        detailRow("playback.replayBrowser.field.duration"_tr().c_str(), formatDuration(replay));
        detailRow("playback.replayBrowser.field.size"_tr().c_str(), formatSize(replay.fileSize));
        detailRow("playback.replayBrowser.field.modified"_tr().c_str(), formatModifiedTime(replay.lastModified));
        detailRow("playback.replayBrowser.field.fileName"_tr().c_str(), replay.replayId);
        ImGui::Spacing();
        ImGui::TextWrapped("%s", "playback.replayBrowser.pathValue"_tr(replay.path.string()).c_str());
        if (!replay.canOpen) {
            ImGui::Spacing();
            ImGui::TextColored(
                ImGui::ColorConvertU32ToFloat4(kColorDanger),
                "%s",
                "playback.replayBrowser.problemValue"_tr(replay.problem).c_str()
            );
        }
        ImGui::EndTooltip();
        ImGui::PopStyleVar();
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
    std::string const worldName =
        replay.worldName.empty() ? "playback.replayBrowser.unknownWorld"_tr() : replay.worldName;
    ImGui::TextDisabled("%s", worldName.c_str());

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

void SelectReplayScreen::drawGrid() {
    if (mVisible.empty()) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 60.0f);
        ImGui::SetWindowFontScale(kFontScaleLarge);
        ImGui::TextDisabled("%s", "playback.replayBrowser.empty"_tr().c_str());
        ImGui::SetWindowFontScale(kFontScaleBody);
        styleButton();
        std::string const importFirstLabel =
            std::string(ICON_EXPORT) + "  " + "playback.replayBrowser.importFirst"_tr();
        if (ImGui::Button(importFirstLabel.c_str(), {240.0f, kControlHeight})) importReplay();
        popButtonStyle();
        return;
    }

    float available = ImGui::GetContentRegionAvail().x - kScreenMargin * 2.0f;
    int   columns   = 5;
    if (available < 5.0f * 240.0f + 4.0f * kCardGap) {
        columns = std::max(1, static_cast<int>((available + kCardGap) / (240.0f + kCardGap)));
    }
    float width = (available - (columns - 1) * kCardGap) / columns;

    // Header count
    ImGui::SetWindowFontScale(kFontScaleSmall);
    ImGui::SetCursorPosX(kScreenMargin);
    ImGui::TextDisabled("%s", "playback.replayBrowser.visibleCount"_tr(mVisible.size()).c_str());
    ImGui::SetWindowFontScale(kFontScaleBody);
    ImGui::Dummy({0.0f, 16.0f});

    ImGui::SetCursorPosX(kScreenMargin);

    for (int item = 0; item < static_cast<int>(mVisible.size()); ++item) {
        if (item > 0 && item % columns != 0) ImGui::SameLine(0.0f, kCardGap);
        if (item % columns == 0 && item > 0) {
            ImGui::SetCursorPosX(kScreenMargin);
        }
        drawCard(replays()[mVisible[static_cast<size_t>(item)]], static_cast<std::size_t>(item), width);
    }
}

void SelectReplayScreen::drawDetails() {
    ImVec2 const available  = ImGui::GetContentRegionAvail();
    bool const   stacked    = available.x < 960.0f;
    float const  gap        = 20.0f;
    float const  listWidth  = stacked ? available.x : std::clamp(available.x * 0.34f, 360.0f, 480.0f);
    float const  listHeight = stacked ? 260.0f : available.y;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorCardBg);
    ImGui::BeginChild("##details-list", {listWidth, listHeight}, false);
    ImGui::SetWindowFontScale(kFontScaleSmall);
    ImGui::TextDisabled("%s", "playback.replayBrowser.fileCount"_tr(mVisible.size()).c_str());
    ImGui::Separator();

    if (mVisible.empty()) {
        ImGui::TextDisabled("%s", "playback.replayBrowser.empty"_tr().c_str());
    } else {
        auto* const font = ImGui::GetFont();
        for (std::size_t visibleIndex = 0; visibleIndex < mVisible.size(); ++visibleIndex) {
            auto const& replay   = replays()[mVisible[visibleIndex]];
            bool const  selected = mSelectedIds.contains(replay.replayId);
            ImGui::PushID(replay.replayId.c_str());
            ImGui::PushStyleColor(ImGuiCol_Header, kColorCardSelected);
            ImGui::Selectable("##detail-item", selected, ImGuiSelectableFlags_AllowDoubleClick, {0.0f, 92.0f});
            ImGui::PopStyleColor();
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
                && ImGui::IsItemHovered()) {
                select(replay.replayId, visibleIndex, false, false);
                openSelected();
            } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                select(replay.replayId, visibleIndex, false, false);
            }
            auto const min = ImGui::GetItemRectMin();
            auto const max = ImGui::GetItemRectMax();
            if (selected) ImGui::GetWindowDrawList()->AddRectFilled(min, {min.x + 4.0f, max.y}, kColorAccent);
            std::string const summary = formatDuration(replay) + "  ·  " + formatSize(replay.fileSize);
            std::string const worldName =
                replay.worldName.empty() ? "playback.replayBrowser.unknownWorld"_tr() : replay.worldName;
            ImGui::GetWindowDrawList()
                ->AddText(font, 24.0f, {min.x + 16.0f, min.y + 8.0f}, kColorText, replay.displayName().c_str());
            ImGui::GetWindowDrawList()
                ->AddText(font, 18.0f, {min.x + 16.0f, min.y + 44.0f}, kColorTextDim, worldName.c_str());
            ImGui::GetWindowDrawList()
                ->AddText(font, 18.0f, {min.x + 16.0f, min.y + 68.0f}, kColorTextDim, summary.c_str());
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

    ImVec2 const panelSize =
        stacked ? ImVec2{available.x, std::max(420.0f, available.y - listHeight - gap)} : ImVec2{0.0f, available.y};
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorCardBg);
    ImGui::BeginChild("##details-panel", panelSize, false);

    auto replay = selectedReplay();
    if (!replay) {
        std::string const selectHint = "playback.replayBrowser.selectForDetails"_tr();
        ImVec2 const      textSize   = ImGui::CalcTextSize(selectHint.c_str());
        ImGui::SetCursorPos({(ImGui::GetWindowWidth() - textSize.x) * 0.5f, 60.0f});
        ImGui::TextDisabled("%s", selectHint.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    // 下方内容全部左对齐，距左边框 50px；元素上下间隔 2px；表格行内文字间隔 0.75px。
    float const margin           = 50.0f;
    float const panelW           = ImGui::GetWindowWidth();
    float const contentW         = panelW - margin * 2.0f;
    float const maxPreviewHeight = std::max(220.0f, ImGui::GetWindowHeight() * 0.53f);
    float       previewWidth     = contentW;
    float       previewHeight    = previewWidth * 9.0f / 16.0f;
    if (previewHeight > maxPreviewHeight) {
        previewHeight = maxPreviewHeight;
        previewWidth  = previewHeight * 16.0f / 9.0f;
    }
    ImGui::SetCursorPos({(panelW - previewWidth) * 0.5f, 24.0f});
    drawPreview(**replay, {previewWidth, previewHeight});

    float y = 24.0f + previewHeight + 2.0f;

    // 标题：最大字号 30px
    ImGui::SetCursorPos({margin, y});
    ImGui::SetWindowFontScale(kFontScaleLarge);
    pushTextColor(kColorText);
    ImGui::TextUnformatted((*replay)->displayName().c_str());
    ImGui::PopStyleColor();
    y += ImGui::GetItemRectSize().y + 2.0f;

    ImGui::SetCursorPos({margin, y});
    ImGui::SetWindowFontScale(kFontScaleSmall);
    ImGui::Separator();
    y += ImGui::GetItemRectSize().y + 2.0f;

    ImGui::SetCursorPos({margin, y});
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {12.0f, 0.75f});
    if (ImGui::BeginTable("##metadata", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##key", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        auto row = [](char const* label, std::string const& value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", label);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(value.c_str());
        };
        row("playback.replayBrowser.field.world"_tr().c_str(),
            (*replay)->worldName.empty() ? "playback.replayBrowser.unknown"_tr() : (*replay)->worldName);
        row("playback.replayBrowser.field.duration"_tr().c_str(), formatDuration(**replay));
        row("playback.replayBrowser.field.fileSize"_tr().c_str(), formatSize((*replay)->fileSize));
        row("playback.replayBrowser.field.fileFormat"_tr().c_str(), ".playback");
        row("playback.replayBrowser.field.fileName"_tr().c_str(), (*replay)->replayId);
        row("playback.replayBrowser.field.filePath"_tr().c_str(), (*replay)->path.string());
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    y += ImGui::GetItemRectSize().y + 2.0f;

    ImGui::SetWindowFontScale(kFontScaleBody);
    ImGui::SetCursorPos({margin, y});
    ImGui::BeginDisabled(!(*replay)->canOpen);
    styleButton();
    if (ImGui::Button("playback.replayBrowser.action.openReplay"_tr().c_str(), {150.0f, kControlHeight})) {
        openSelected();
    }
    popButtonStyle();
    ImGui::EndDisabled();
    ImGui::SameLine();
    styleButton();
    if (ImGui::Button("playback.replayBrowser.action.copyPath"_tr().c_str(), {140.0f, kControlHeight})) {
        ImGui::SetClipboardText((*replay)->path.string().c_str());
    }
    popButtonStyle();
    ImGui::SameLine();
    styleButton();
    if (ImGui::Button("playback.replayBrowser.action.showInFolder"_tr().c_str(), {160.0f, kControlHeight})) {
        playback::editor::EditorAction action{playback::editor::EditorActionType::ShowReplayInFolder};
        action.replayId = (*replay)->replayId;
        submit(std::move(action));
    }
    popButtonStyle();
    ImGui::SameLine();
    styleButton();
    if (ImGui::Button("playback.replayBrowser.action.delete"_tr().c_str(), {110.0f, kControlHeight})) {
        mShowDeleteDialog = true;
    }
    popButtonStyle();

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void SelectReplayScreen::drawActionBar() {
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
        ImGui::TextUnformatted("playback.replayBrowser.selectedCount"_tr(mSelectedIds.size()).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::SetWindowFontScale(kFontScaleBody);

    float const right = contentW - 24.0f;
    float const btnW  = 120.0f;
    ImGui::SetCursorPos({right - btnW * 3.0f - 24.0f, 20.0f});

    ImGui::BeginDisabled(!replay || !(*replay)->canOpen);
    styleButton();
    if (ImGui::Button("playback.replayBrowser.action.open"_tr().c_str(), {btnW, kControlHeight})) openSelected();
    popButtonStyle();
    ImGui::SameLine();
    styleButton();
    if (ImGui::Button("playback.replayBrowser.action.edit"_tr().c_str(), {btnW, kControlHeight})) openRenameDialog();
    popButtonStyle();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, kColorDanger);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorDanger);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColorDanger);
    if (ImGui::Button("playback.replayBrowser.action.delete"_tr().c_str(), {btnW, kControlHeight})) {
        mShowDeleteDialog = true;
    }
    ImGui::PopStyleColor(3);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void SelectReplayScreen::drawDeleteDialog() {
    std::string const deleteTitle = "playback.replayBrowser.dialog.delete.title"_tr() + "###delete-replay";
    if (mShowDeleteDialog) ImGui::OpenPopup(deleteTitle.c_str());
    if (ImGui::BeginPopupModal(deleteTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("playback.replayBrowser.dialog.delete.confirm"_tr().c_str());
        ImGui::TextDisabled("%s", "playback.replayBrowser.dialog.delete.irreversible"_tr().c_str());
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, kColorDanger);
        std::string const confirmDelete =
            std::string(ICON_DELETE) + "  " + "playback.replayBrowser.dialog.delete.confirmButton"_tr();
        if (ImGui::Button(confirmDelete.c_str(), {156.0f, kControlHeight})) {
            playback::editor::EditorAction action{playback::editor::EditorActionType::DeleteReplays};
            action.replayIds.assign(mSelectedIds.begin(), mSelectedIds.end());
            submit(std::move(action));
            mShowDeleteDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        std::string const cancel = std::string(ICON_CLOSE) + "  " + "playback.replayBrowser.dialog.cancel"_tr();
        if (ImGui::Button(cancel.c_str(), {120.0f, kControlHeight})) {
            mShowDeleteDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (mState && !mState->error.empty()) {
        std::string const errorTitle =
            "playback.replayBrowser.dialog.operationFailed"_tr() + "###replay-operation-failed";
        ImGui::OpenPopup(errorTitle.c_str());
        if (ImGui::BeginPopupModal(errorTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", mState->error.c_str());
            std::string const ok = std::string(ICON_CHECK) + "  " + "playback.replayBrowser.dialog.ok"_tr();
            if (ImGui::Button(ok.c_str(), {120.0f, kControlHeight})) {
                submit({playback::editor::EditorActionType::ClearReplayBrowserError});
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

void SelectReplayScreen::openRenameDialog() {
    auto replay = selectedReplay();
    if (!replay || !(*replay)->canOpen) return;
    mRenameBuffer     = (*replay)->displayName();
    mRenameDialogOpen = true;
}

void SelectReplayScreen::drawRenameDialog() {
    auto              replay      = selectedReplay();
    std::string const renameTitle = "playback.replayBrowser.dialog.rename.title"_tr() + "###rename-replay";
    if (mRenameDialogOpen) {
        ImGui::OpenPopup(renameTitle.c_str());
        mRenameDialogOpen = false;
    }
    if (!ImGui::BeginPopupModal(renameTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::SetWindowFontScale(kFontScaleBody);
    ImGui::TextDisabled("%s", "playback.replayBrowser.dialog.rename.description"_tr().c_str());
    ImGui::Spacing();

    std::array<char, 256> buffer{};
    std::copy_n(mRenameBuffer.data(), std::min(mRenameBuffer.size(), buffer.size() - 1), buffer.data());
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kColorButton);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kColorButtonHover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kColorButtonActive);
    ImGui::PushStyleColor(ImGuiCol_Text, kColorText);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12.0f, 10.0f});
    ImGui::SetNextItemWidth(460.0f);
    bool const edited =
        ImGui::InputText("##rename-input", buffer.data(), buffer.size(), ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    if (edited) mRenameBuffer = buffer.data();

    ImGui::Spacing();
    bool const empty = mRenameBuffer.empty();
    ImGui::BeginDisabled(empty);
    styleButton();
    std::string const save  = std::string(ICON_CHECK) + "  " + "playback.replayBrowser.dialog.rename.save"_tr();
    bool const        saved = ImGui::Button(save.c_str(), {140.0f, kControlHeight});
    popButtonStyle();
    ImGui::EndDisabled();
    ImGui::SameLine();
    styleButton();
    std::string const cancel    = std::string(ICON_CLOSE) + "  " + "playback.replayBrowser.dialog.cancel"_tr();
    bool const        cancelled = ImGui::Button(cancel.c_str(), {120.0f, kControlHeight});
    popButtonStyle();

    bool const confirm = saved;
    if (confirm && replay) {
        playback::editor::EditorAction action{playback::editor::EditorActionType::RenameReplay};
        action.replayId = (*replay)->replayId;
        action.name     = mRenameBuffer;
        submit(std::move(action));
        ImGui::CloseCurrentPopup();
    } else if (cancelled) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace playback::screen::select_replay
