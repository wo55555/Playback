#include "ReplayBrowserWindow.h"

#include "playback/refactor/editor/EditorTheme.h"
#include "playback/functions/render/ReplayThumbnail.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <unordered_map>

namespace playback::refactor::replay_browser {

namespace {

constexpr float kNavigationHeight = 56.0f;
constexpr float kActionBarHeight  = 64.0f;
constexpr float kCardMinWidth     = 240.0f;
constexpr float kCardGap          = 16.0f;

struct ThumbnailCacheEntry {
    bool attempted{};
    functions::render::ReplayThumbnailPixels pixels;
};

std::unordered_map<std::string, ThumbnailCacheEntry> gThumbnailCache;

} // namespace

ReplayBrowserWindow& ReplayBrowserWindow::getInstance() {
    static ReplayBrowserWindow instance;
    return instance;
}

void ReplayBrowserWindow::open() {
    refresh();
    mOpen = true;
}

void ReplayBrowserWindow::close() {
    mOpen = false;
    mSelectedIndex.reset();
}

bool ReplayBrowserWindow::isOpen() const { return mOpen; }

bool ReplayBrowserWindow::ownsInput() const { return mOpen; }

void ReplayBrowserWindow::refresh() {
    mReplays = screen::ReplayBrowser::loadReplays();
    mSelectedIndex.reset();
}

void ReplayBrowserWindow::draw() {
    if (!mOpen) return;

    editor::EditorTheme theme;
    theme.apply();

    auto const& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(12, 16, 24, 255));
    ImGui::Begin(
        "##replay-browser",
        nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoBringToFrontOnFocus
    );

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
    drawNavigation();
    drawContent();
    if (mSelectedIndex.has_value()) drawActionBar();

    ImGui::End();
    ImGui::PopStyleColor();
}

void ReplayBrowserWindow::drawNavigation() {
    ImGui::BeginChild("##replay-browser-navigation", {0.0f, kNavigationHeight}, false);
    if (ImGui::Button("< 回放")) close();
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 24.0f, ImGui::GetWindowWidth() - 520.0f));
    std::array<char, 256> search{};
    std::copy_n(mSearch.c_str(), std::min(mSearch.size(), search.size() - 1), search.data());
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::InputTextWithHint("##replay-browser-search", "搜索回放文件", search.data(), search.size())) {
        mSearch = search.data();
    }
    ImGui::SameLine();
    if (ImGui::Button("刷新")) refresh();
    ImGui::SameLine();
    ImGui::BeginDisabled();
    ImGui::Button("导入");
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::Separator();
}

void ReplayBrowserWindow::drawContent() {
    float const actionHeight = mSelectedIndex.has_value() ? kActionBarHeight : 0.0f;
    ImGui::BeginChild("##replay-browser-content", {0.0f, -actionHeight}, false);

    std::vector<screen::ReplaySummary const*> visible;
    visible.reserve(mReplays.size());
    for (auto const& replay : mReplays) {
        if (mSearch.empty() || replay.matches(mSearch)) visible.push_back(&replay);
    }

    ImGui::Text("共 %zu 个文件", visible.size());
    if (visible.empty()) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 32.0f);
        ImGui::TextDisabled("暂无回放文件");
        ImGui::EndChild();
        return;
    }

    float const availableWidth = ImGui::GetContentRegionAvail().x;
    int const columns = std::max(1, static_cast<int>((availableWidth + kCardGap) / (kCardMinWidth + kCardGap)));
    float const cardWidth = (availableWidth - (columns - 1) * kCardGap) / columns;
    for (std::size_t i = 0; i < visible.size(); ++i) {
        auto const index = static_cast<std::size_t>(visible[i] - mReplays.data());
        drawCard(*visible[i], index, cardWidth);
        if ((i + 1) % static_cast<std::size_t>(columns) != 0) ImGui::SameLine(0.0f, kCardGap);
    }
    ImGui::EndChild();
}

void ReplayBrowserWindow::drawCard(screen::ReplaySummary const& replay, std::size_t index, float width) {
    bool const selected = mSelectedIndex == index;
    ImGui::PushID(static_cast<int>(index));
    ImGui::PushStyleColor(
        ImGuiCol_ChildBg,
        selected ? IM_COL32(25, 43, 70, 255) : IM_COL32(22, 30, 43, 255)
    );
    ImGui::BeginChild("##replay-card", {width, width * 0.75f + 82.0f}, true);
    auto const imageStart = ImGui::GetCursorScreenPos();
    auto const imageEnd = ImVec2(imageStart.x + ImGui::GetContentRegionAvail().x, imageStart.y + width * 0.75f - 12.0f);
    ImGui::GetWindowDrawList()->AddRectFilled(imageStart, imageEnd, ImColor(36, 52, 75, 255), 4.0f);
    auto& thumbnail = gThumbnailCache[replay.replayId];
    if (!thumbnail.attempted) {
        thumbnail.attempted = true;
        if (!replay.thumbnailPng.empty()) {
            auto const decoded = functions::render::decodeReplayThumbnailPng(replay.thumbnailPng, thumbnail.pixels);
            static_cast<void>(decoded);
        }
    }
    ImGui::Dummy({0.0f, width * 0.75f - 12.0f});
    if (ImGui::Selectable(replay.displayName().c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
        mSelectedIndex = index;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && replay.canOpen && screen::ReplayBrowser::openReplay(replay)) {
            close();
        }
    }
    ImGui::TextDisabled("%s", replay.worldName.empty() ? "-" : replay.worldName.c_str());
    ImGui::TextDisabled("%llu 字节", static_cast<unsigned long long>(replay.fileSize));
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

std::optional<screen::ReplaySummary const*> ReplayBrowserWindow::selectedReplay() const {
    if (!mSelectedIndex.has_value() || *mSelectedIndex >= mReplays.size()) return std::nullopt;
    return &mReplays[*mSelectedIndex];
}

void ReplayBrowserWindow::drawActionBar() {
    auto replay = selectedReplay();
    if (!replay.has_value()) return;

    ImGui::BeginChild("##replay-browser-actions", {0.0f, kActionBarHeight}, true);
    ImGui::Text("%s", (*replay)->displayName().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%llu 字节", static_cast<unsigned long long>((*replay)->fileSize));
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 24.0f, ImGui::GetWindowWidth() - 240.0f));
    if (ImGui::Button("打开") && (*replay)->canOpen && screen::ReplayBrowser::openReplay(**replay)) close();
    ImGui::SameLine();
    ImGui::BeginDisabled();
    ImGui::Button("编辑");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled();
    ImGui::Button("删除");
    ImGui::EndDisabled();
    ImGui::EndChild();
}

} // namespace playback::refactor::replay_browser
