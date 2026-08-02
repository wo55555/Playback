#include "TimelinePanel.h"

#include "playback/editor/ui/ReplayEditor.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace playback::editor::ui {

namespace {

std::string formatTick(int tick) {
    char value[32]{};
    std::snprintf(value, sizeof(value), "%02d:%02d", std::max(0, tick) / 1200, (std::max(0, tick) / 20) % 60);
    return value;
}

ImU32 color(editing::model::Color4 const& value, int alpha = 220) {
    return IM_COL32(static_cast<int>(value.r * 255.0f), static_cast<int>(value.g * 255.0f), static_cast<int>(value.b * 255.0f), alpha);
}

}

void TimelinePanel::setViewPreferences(float trackListWidthRatio, float pixelsPerTick, float horizontalScroll) {
    mTrackListWidthRatio = std::clamp(trackListWidthRatio, 0.18f, 0.55f);
    mPixelsPerTick = std::clamp(pixelsPerTick, 0.02f, 8.0f);
    mScrollX = std::max(0.0f, horizontalScroll);
}

void TimelinePanel::submitSeek(int tick) {
    auto const& state = ReplayEditor::getInstance().state();
    mPendingSeekTick = std::clamp(tick, 0, std::max(0, state.totalTicks));
    EditorAction action{EditorActionType::Seek};
    action.tick = mPendingSeekTick;
    ReplayEditor::getInstance().submitAction(std::move(action));
}

void TimelinePanel::submitEdit(EditorAction action) {
    ReplayEditor::getInstance().submitAction(std::move(action));
}

void TimelinePanel::draw() {
    auto& editor = ReplayEditor::getInstance();
    auto const& state = editor.state();
    auto const project = state.project;
    if (!project) {
        ImGui::TextDisabled("No replay project is active.");
        return;
    }

    mTrackTree.setSearch(mTrackSearch);
    mTrackTree.rebuild(*project);
    int displayTick = mPendingSeekTick >= 0 ? mPendingSeekTick : state.currentTick;
    if (mPendingSeekTick >= 0 && state.currentTick == mPendingSeekTick) mPendingSeekTick = -1;

    ImGui::Text("%s / %s", formatTick(displayTick).c_str(), formatTick(state.totalTicks).c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.canUndo);
    if (ImGui::Button("Undo")) submitEdit({EditorActionType::UndoEditorEdit});
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.canRedo);
    if (ImGui::Button("Redo")) submitEdit({EditorActionType::RedoEditorEdit});
    ImGui::EndDisabled();
    ImGui::SameLine();
    auto const* selectedCamera = editor.selection().getAs<editing::model::SelectedCamera>();
    ImGui::BeginDisabled(selectedCamera == nullptr);
    if (ImGui::Button("+ Key")) {
        EditorAction action{EditorActionType::AddCameraKeyframe};
        action.id = selectedCamera->cameraId;
        action.tick = state.currentTick;
        submitEdit(std::move(action));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("+ Camera")) {
        EditorAction action{EditorActionType::AddFreeCamera};
        action.name = "Camera " + std::to_string(project->cameras.size() + 1);
        submitEdit(std::move(action));
    }
    ImGui::SameLine();
    if (ImGui::Button("-")) mPixelsPerTick = std::max(0.02f, mPixelsPerTick * 0.9f);
    ImGui::SameLine();
    if (ImGui::Button("+")) mPixelsPerTick = std::min(8.0f, mPixelsPerTick * 1.1f);

    ImVec2 available = ImGui::GetContentRegionAvail();
    if (available.x < 120.0f || available.y < 80.0f) return;
    float labelWidth = std::clamp(available.x * mTrackListWidthRatio, 150.0f, available.x * 0.55f);
    float timelineWidth = std::max(1.0f, available.x - labelWidth);
    float rulerHeight = 28.0f;
    float bodyHeight = std::max(1.0f, available.y - rulerHeight - 20.0f);
    float contentWidth = std::max(timelineWidth, state.totalTicks * mPixelsPerTick);
    float maxScroll = std::max(0.0f, contentWidth - timelineWidth);
    mScrollX = std::clamp(mScrollX, 0.0f, maxScroll);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    auto* drawList = ImGui::GetWindowDrawList();

    char search[128]{};
    std::snprintf(search, sizeof(search), "%s", mTrackSearch.c_str());
    ImGui::SetCursorScreenPos(origin);
    ImGui::SetNextItemWidth(labelWidth - 8.0f);
    if (ImGui::InputTextWithHint("##timeline-search", "Search cameras", search, sizeof(search))) mTrackSearch = search;

    float rulerTop = origin.y + 30.0f;
    float bodyTop = rulerTop + rulerHeight;
    float bodyBottom = bodyTop + bodyHeight - 30.0f;
    float timelineLeft = origin.x + labelWidth;
    float timelineRight = timelineLeft + timelineWidth;
    drawList->AddRectFilled({timelineLeft, rulerTop}, {timelineRight, bodyBottom}, IM_COL32(25, 26, 31, 255));
    int majorStep = std::max(20, static_cast<int>(std::ceil(100.0f / mPixelsPerTick / 20.0f)) * 20);
    int firstTick = std::max(0, static_cast<int>(mScrollX / mPixelsPerTick / majorStep) * majorStep);
    for (int tick = firstTick; tick <= state.totalTicks; tick += majorStep) {
        float x = timelineLeft + tick * mPixelsPerTick - mScrollX;
        if (x < timelineLeft || x > timelineRight) continue;
        drawList->AddLine({x, rulerTop + 15.0f}, {x, rulerTop + rulerHeight}, IM_COL32(135, 140, 150, 255));
        drawList->AddText({x + 3.0f, rulerTop + 1.0f}, IM_COL32(185, 190, 200, 255), formatTick(tick).c_str());
    }

    ImGui::SetCursorScreenPos({timelineLeft, rulerTop});
    ImGui::InvisibleButton("##timeline-ruler", {timelineWidth, rulerHeight});
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        displayTick = std::clamp(static_cast<int>((ImGui::GetIO().MousePos.x - timelineLeft + mScrollX) / mPixelsPerTick), 0, state.totalTicks);
    }
    if (ImGui::IsItemDeactivated()) submitSeek(displayTick);

    float y = bodyTop + 2.0f;
    for (auto const& row : mTrackTree.rows()) {
        float rowBottom = y + row.height;
        drawList->AddRectFilled({origin.x, y}, {timelineRight, rowBottom}, IM_COL32(34, 35, 41, 255));
        drawList->AddText({origin.x + 8.0f, y + 4.0f}, IM_COL32(210, 213, 220, 255), row.name.c_str());
        if (row.kind == editing::model::TrackRowKind::Sequence) {
            for (auto const& segment : project->sequence) {
                ImVec2 minimum{timelineLeft + segment.startTick * mPixelsPerTick - mScrollX, y + 4.0f};
                ImVec2 maximum{timelineLeft + segment.endTick * mPixelsPerTick - mScrollX, rowBottom - 4.0f};
                bool selected = editor.selection().getAs<editing::model::SelectedSequenceSegment>() && editor.selection().getAs<editing::model::SelectedSequenceSegment>()->segmentId == segment.id;
                drawList->AddRectFilled(minimum, maximum, color(segment.color), 3.0f);
                drawList->AddRect(minimum, maximum, selected ? IM_COL32(240, 192, 32, 255) : IM_COL32(110, 170, 240, 255), 3.0f);
                drawList->AddText({minimum.x + 5.0f, minimum.y + 5.0f}, IM_COL32(240, 240, 242, 255), segment.cameraId.empty() ? "Auto" : segment.cameraId.c_str());
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetMousePos().x >= minimum.x && ImGui::GetMousePos().x <= maximum.x && ImGui::GetMousePos().y >= minimum.y && ImGui::GetMousePos().y <= maximum.y) editor.selection().select(editing::model::SelectedSequenceSegment{segment.id});
            }
        } else if (row.kind == editing::model::TrackRowKind::WorldActor) {
            for (auto const& segment : project->worldActor.segments) {
                ImVec2 minimum{timelineLeft + segment.startTick * mPixelsPerTick - mScrollX, y + 4.0f};
                ImVec2 maximum{timelineLeft + segment.endTick * mPixelsPerTick - mScrollX, rowBottom - 4.0f};
                bool selected = editor.selection().getAs<editing::model::SelectedWorldActorSegment>() && editor.selection().getAs<editing::model::SelectedWorldActorSegment>()->segmentId == segment.id;
                drawList->AddRectFilled(minimum, maximum, color(segment.color), 3.0f);
                drawList->AddRect(minimum, maximum, selected ? IM_COL32(240, 192, 32, 255) : IM_COL32(240, 150, 80, 255), 3.0f);
                char label[32]{};
                std::snprintf(label, sizeof(label), "%.2fx", segment.speed);
                drawList->AddText({minimum.x + 5.0f, minimum.y + 5.0f}, IM_COL32(240, 240, 242, 255), label);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetMousePos().x >= minimum.x && ImGui::GetMousePos().x <= maximum.x && ImGui::GetMousePos().y >= minimum.y && ImGui::GetMousePos().y <= maximum.y) editor.selection().select(editing::model::SelectedWorldActorSegment{segment.id});
            }
        } else if (row.kind == editing::model::TrackRowKind::Camera && row.cameraIndex >= 0 && row.cameraIndex < static_cast<int>(project->cameras.size())) {
            auto const& camera = project->cameras[row.cameraIndex];
            for (auto const& key : camera.keys) {
                float x = timelineLeft + key.tick * mPixelsPerTick - mScrollX;
                drawList->AddCircleFilled({x, (y + rowBottom) * 0.5f}, 5.0f, IM_COL32(128, 192, 240, 255));
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && std::abs(ImGui::GetMousePos().x - x) <= 7.0f && ImGui::GetMousePos().y >= y && ImGui::GetMousePos().y <= rowBottom) editor.selection().select(editing::model::SelectedKeyframe{camera.id, key.id});
            }
        } else if (row.kind == editing::model::TrackRowKind::Marker) {
            for (auto const& marker : project->markers) {
                float x = timelineLeft + marker.tick * mPixelsPerTick - mScrollX;
                drawList->AddLine({x, y}, {x, rowBottom}, IM_COL32(240, 192, 32, 255));
                drawList->AddText({x + 4.0f, y + 3.0f}, IM_COL32(240, 210, 100, 255), marker.label.c_str());
            }
        }
        y = rowBottom + 2.0f;
    }
    float playheadX = std::clamp(timelineLeft + displayTick * mPixelsPerTick - mScrollX, timelineLeft, timelineRight);
    drawList->AddLine({playheadX, rulerTop}, {playheadX, bodyBottom}, IM_COL32(240, 192, 32, 255), 2.0f);
    ImGui::SetCursorScreenPos({timelineLeft, bodyBottom + 3.0f});
    if (maxScroll > 0.0f) {
        ImGui::SetNextItemWidth(timelineWidth);
        ImGui::SliderFloat("##timeline-scroll", &mScrollX, 0.0f, maxScroll, "", ImGuiSliderFlags_NoInput);
    }
}

} // namespace playback::editor::ui
