#include "TimelinePanel.h"

#include "playback/editor/ui/ReplayEditor.h"
#include "playback/editor/ui/iconfont.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace playback::editor::ui {

namespace {

std::string formatTick(int tick) {
    int const seconds = std::max(0, tick) / 20;
    char      value[32]{};
    std::snprintf(value, sizeof(value), "%02d:%02d", seconds / 60, seconds % 60);
    return value;
}

bool toolbarButton(char const* label, char const* tooltip) {
    bool const clicked = ImGui::Button(label, {30.0f, 28.0f});
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

} // namespace

void TimelinePanel::setViewPreferences(float trackListWidthRatio, float pixelsPerTick, float horizontalScroll) {
    mTrackListWidthRatio = std::clamp(trackListWidthRatio, 0.18f, 0.55f);
    mPixelsPerTick       = std::clamp(pixelsPerTick, 0.02f, 8.0f);
    mScrollX             = std::max(0.0f, horizontalScroll);
}

void TimelinePanel::submitSeek(int tick) {
    auto const& state = ReplayEditor::getInstance().state();
    mPendingSeekTick  = std::clamp(tick, 0, std::max(0, state.totalTicks));
    playback::editor::EditorAction action{playback::editor::EditorActionType::Seek};
    action.tick = mPendingSeekTick;
    ReplayEditor::getInstance().submitAction(std::move(action));
}

void TimelinePanel::draw() {
    auto&       editor = ReplayEditor::getInstance();
    auto const& state  = editor.state();

    if (mPendingSeekTick >= 0 && state.currentTick == mPendingSeekTick) mPendingSeekTick = -1;
    int const displayTick = mPendingSeekTick >= 0 ? mPendingSeekTick : state.currentTick;

    if (toolbarButton(ICON_RESET, "Skip to start")) {
        editor.submitAction({playback::editor::EditorActionType::SkipToStart});
    }
    ImGui::SameLine();
    if (toolbarButton(ICON_BACK, "Decrease playback speed")) {
        editor.submitAction({playback::editor::EditorActionType::DecreaseSpeed});
    }
    ImGui::SameLine();
    if (toolbarButton(state.paused ? ICON_PLAY : ICON_PAUSE, state.paused ? "Play" : "Pause")) {
        editor.submitAction({playback::editor::EditorActionType::TogglePause});
    }
    ImGui::SameLine();
    if (toolbarButton(">", "Increase playback speed")) {
        editor.submitAction({playback::editor::EditorActionType::IncreaseSpeed});
    }
    ImGui::SameLine();
    if (toolbarButton(">|", "Skip to end")) {
        editor.submitAction({playback::editor::EditorActionType::SkipToEnd});
    }
    ImGui::SameLine();
    ImGui::Text("%s / %s", formatTick(displayTick).c_str(), formatTick(state.totalTicks).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%.2fx", state.playbackSpeed);

    ImVec2 const available = ImGui::GetContentRegionAvail();
    if (available.x <= 1.0f || available.y <= 1.0f) return;

    float const labelWidth    = std::clamp(available.x * mTrackListWidthRatio, 150.0f, available.x * 0.55f);
    float const rulerHeight   = 28.0f;
    float const scrollHeight  = 20.0f;
    float const timelineLeft  = ImGui::GetCursorScreenPos().x + labelWidth;
    float const timelineWidth = std::max(1.0f, available.x - labelWidth);
    float const timelineRight = timelineLeft + timelineWidth;
    float const rulerTop      = ImGui::GetCursorScreenPos().y;
    float const bodyBottom    = rulerTop + std::max(rulerHeight + 1.0f, available.y - scrollHeight);

    float const contentWidth = std::max(timelineWidth, state.totalTicks * mPixelsPerTick);
    float const maxScroll    = std::max(0.0f, contentWidth - timelineWidth);
    mScrollX                 = std::clamp(mScrollX, 0.0f, maxScroll);

    auto* drawList = ImGui::GetWindowDrawList();
    drawList
        ->AddRectFilled({timelineLeft, rulerTop}, {timelineRight, rulerTop + rulerHeight}, IM_COL32(30, 31, 36, 255));
    drawList
        ->AddRectFilled({timelineLeft, rulerTop + rulerHeight}, {timelineRight, bodyBottom}, IM_COL32(24, 25, 29, 255));
    drawList->AddRectFilled(
        {timelineLeft, rulerTop + rulerHeight + 2.0f},
        {timelineRight, rulerTop + rulerHeight + 42.0f},
        IM_COL32(37, 39, 45, 255)
    );

    int const majorStep =
        std::max(20, static_cast<int>(std::ceil(100.0f / std::max(0.02f, mPixelsPerTick) / 20.0f)) * 20);
    int const firstTick = std::max(0, static_cast<int>(mScrollX / mPixelsPerTick / majorStep) * majorStep);
    for (int tick = firstTick; tick <= state.totalTicks; tick += majorStep) {
        float const x = timelineLeft + tick * mPixelsPerTick - mScrollX;
        if (x < timelineLeft || x > timelineRight) continue;
        drawList->AddLine({x, rulerTop + 16.0f}, {x, rulerTop + rulerHeight}, IM_COL32(130, 134, 145, 255));
        drawList->AddText({x + 4.0f, rulerTop + 2.0f}, IM_COL32(180, 184, 194, 255), formatTick(tick).c_str());
    }

    float const playheadX =
        std::clamp(timelineLeft + displayTick * mPixelsPerTick - mScrollX, timelineLeft, timelineRight);
    drawList->AddLine({playheadX, rulerTop}, {playheadX, bodyBottom}, IM_COL32(240, 192, 32, 255), 2.0f);
    drawList->AddTriangleFilled(
        {playheadX - 6.0f, rulerTop},
        {playheadX + 6.0f, rulerTop},
        {playheadX, rulerTop + 8.0f},
        IM_COL32(240, 192, 32, 255)
    );

    drawList->AddText(
        {ImGui::GetCursorScreenPos().x + 10.0f, rulerTop + rulerHeight + 13.0f},
        IM_COL32(150, 154, 165, 255),
        state.capabilities.videoEditing ? "Video tracks" : "Video tracks (backend unavailable)"
    );
    drawList->AddText(
        {ImGui::GetCursorScreenPos().x + 10.0f, rulerTop + rulerHeight + 55.0f},
        IM_COL32(150, 154, 165, 255),
        state.capabilities.cameraEditing ? "Camera tracks" : "Camera tracks (backend unavailable)"
    );

    ImGui::SetCursorScreenPos({timelineLeft, rulerTop});
    ImGui::InvisibleButton("##playback-ruler", {timelineWidth, rulerHeight});
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float const relative = ImGui::GetIO().MousePos.x - timelineLeft + mScrollX;
        mPendingSeekTick     = std::clamp(
            static_cast<int>(std::lround(relative / std::max(0.02f, mPixelsPerTick))),
            0,
            std::max(0, state.totalTicks)
        );
    }
    if (ImGui::IsItemDeactivated() && mPendingSeekTick >= 0) submitSeek(mPendingSeekTick);

    if (ImGui::IsWindowHovered()) {
        float const wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            if (ImGui::GetIO().KeyCtrl) {
                mPixelsPerTick = std::clamp(mPixelsPerTick * (wheel > 0.0f ? 1.15f : 0.87f), 0.02f, 8.0f);
            } else {
                mScrollX = std::clamp(mScrollX - wheel * 80.0f, 0.0f, maxScroll);
            }
        }
    }

    ImGui::SetCursorScreenPos({timelineLeft, bodyBottom + 1.0f});
    if (maxScroll > 0.0f) {
        ImGui::SetNextItemWidth(timelineWidth);
        ImGui::SliderFloat("##timeline-scroll", &mScrollX, 0.0f, maxScroll, "", ImGuiSliderFlags_NoInput);
    } else {
        ImGui::Dummy({timelineWidth, 1.0f});
    }
}

} // namespace playback::editor::ui
