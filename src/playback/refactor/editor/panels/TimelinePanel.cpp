#include "TimelinePanel.h"

#include "playback/refactor/editor/Editor.h"
#include "playback/refactor/editor/HintBar.h"
#include "playback/refactor/editor/iconfont.h"
#include "playback/refactor/editor/models/SelectionModel.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

namespace playback::refactor::editor {

void TimelinePanel::draw() {
    ImGui::Begin("Timeline", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    drawHeader();
    drawBody();

    // Hint bar at bottom
    ImGui::Separator();
    HintBar hintBar;
    hintBar.draw();

    ImGui::End();
}

void TimelinePanel::drawHeader() {
    auto& state = Editor::getInstance().state();

    // Play button
    if (ImGui::Button(state.playing ? ICON_PAUSE : ICON_PLAY)) {
        state.playing = !state.playing;
    }
    ImGui::SameLine();

    // Timecode display
    int totalSeconds = state.currentTick / 20;
    int hours   = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int secs    = totalSeconds % 60;
    int millis  = (state.currentTick % 20) * 50; // 20 ticks/sec = 50ms per tick

    int totalDuration = state.totalTicks / 20;
    int durHours   = totalDuration / 3600;
    int durMinutes = (totalDuration % 3600) / 60;
    int durSecs    = totalDuration % 60;
    int durMillis  = (state.totalTicks % 20) * 50;

    char timecode[64];
    std::snprintf(timecode, sizeof(timecode), "%02d:%02d:%02d.%03d / %02d:%02d:%02d.%03d",
        hours, minutes, secs, millis,
        durHours, durMinutes, durSecs, durMillis);
    ImGui::TextUnformatted(timecode);
    ImGui::SameLine();

    // Add keyframe button
    if (ImGui::Button(ICON_ADD_KEYFRAME)) {
        // Placeholder
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Add Keyframe at Playhead (K)");
    }
    ImGui::SameLine();

    // Add marker button
    if (ImGui::Button(ICON_ADD_MARKER)) {
        // Placeholder
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Add Marker (M)");
    }
    ImGui::SameLine();

    // Split button
    if (ImGui::Button(ICON_SPLIT)) {
        // Placeholder
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Split Clip at Playhead (Ctrl+K)");
    }
    ImGui::SameLine();

    // Zoom controls
    ImGui::Text("Zoom: ");
    ImGui::SameLine();
    if (ImGui::Button("-")) {
        mPixelsPerTick /= 1.1f;
        mPixelsPerTick = std::clamp(mPixelsPerTick, 0.02f, 2.0f);
    }
    ImGui::SameLine();
    ImGui::Text("%.1fx", mPixelsPerTick / 0.1f);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        mPixelsPerTick = 0.1f;
    }
    ImGui::SameLine();
    if (ImGui::Button("+")) {
        mPixelsPerTick *= 1.1f;
        mPixelsPerTick = std::clamp(mPixelsPerTick, 0.02f, 2.0f);
    }

    // Handle scrub on header area
    Rect headerArea;
    headerArea.min = ImGui::GetItemRectMin();
    headerArea.max = ImGui::GetItemRectMax();
    // Extend header area to full width for scrub handling
    headerArea.max.x = ImGui::GetWindowContentRegionMax().x + ImGui::GetWindowPos().x;
    handleScrubDrag(headerArea);
}

void TimelinePanel::drawBody() {
    auto& state = Editor::getInstance().state();

    float bodyWidth  = ImGui::GetContentRegionAvail().x;
    float bodyHeight = ImGui::GetContentRegionAvail().y;

    // Video track (V0) - 48px
    float trackStartY = ImGui::GetCursorScreenPos().y;
    {
        Rect rowArea;
        rowArea.min = ImGui::GetCursorScreenPos();
        rowArea.max = ImVec2(rowArea.min.x + bodyWidth, rowArea.min.y + 48.0f);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(rowArea.min, rowArea.max, IM_COL32(0x1e, 0x1e, 0x1e, 0xff));
        dl->AddRect(rowArea.min, rowArea.max, IM_COL32(0x3a, 0x3a, 0x3a, 0xff));

        // Track label
        ImGui::SetCursorScreenPos(rowArea.min);
        ImGui::Text("V0");

        // Draw clips
        for (auto& clip : state.videoClips) {
            drawVideoClip(clip, rowArea);
        }

        ImGui::SetCursorScreenPos(ImVec2(rowArea.min.x, rowArea.max.y));
    }

    // Camera tracks (Cn) - 24px each
    for (auto& track : state.cameraTracks) {
        Rect rowArea;
        rowArea.min = ImGui::GetCursorScreenPos();
        rowArea.max = ImVec2(rowArea.min.x + bodyWidth, rowArea.min.y + 24.0f);

        TrackDescriptor desc;
        desc.type    = TrackType::Camera;
        desc.id      = track.id;
        desc.name    = track.name;
        desc.active  = track.active;
        desc.locked  = false;
        desc.muted   = false;
        desc.visible = true;

        drawTrack(desc, rowArea);

        // Draw keyframes on this track
        for (auto& kf : track.keyframes) {
            drawKeyframe(kf, rowArea);
        }

        ImGui::SetCursorScreenPos(ImVec2(rowArea.min.x, rowArea.max.y));
    }

    // Marker track (M) - 20px
    {
        Rect rowArea;
        rowArea.min = ImGui::GetCursorScreenPos();
        rowArea.max = ImVec2(rowArea.min.x + bodyWidth, rowArea.min.y + 20.0f);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(rowArea.min, rowArea.max, IM_COL32(0x18, 0x18, 0x18, 0xff));
        dl->AddRect(rowArea.min, rowArea.max, IM_COL32(0x3a, 0x3a, 0x3a, 0xff));

        ImGui::SetCursorScreenPos(rowArea.min);
        ImGui::Text("M");

        for (auto& marker : state.markers) {
            drawMarker(marker, rowArea);
        }

        ImGui::SetCursorScreenPos(ImVec2(rowArea.min.x, rowArea.max.y));
    }
}

void TimelinePanel::drawTrack(const TrackDescriptor& track, Rect rowArea) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Track row background
    dl->AddRectFilled(rowArea.min, rowArea.max, track.active ? IM_COL32(0x22, 0x22, 0x28, 0xff) : IM_COL32(0x1a, 0x1a, 0x1a, 0xff));
    dl->AddRect(rowArea.min, rowArea.max, IM_COL32(0x3a, 0x3a, 0x3a, 0xff));

    // Track label
    ImGui::SetCursorScreenPos(ImVec2(rowArea.min.x + 4, rowArea.min.y + 2));
    ImGui::Text("%s %s", track.active ? ICON_TRACK_ACTIVE : ICON_TRACK_OFF, track.name.c_str());

    // Track progress line
    if (track.active) {
        float lineY = (rowArea.min.y + rowArea.max.y) * 0.5f;
        float lineStartX = rowArea.min.x + 80.0f;
        float lineEndX   = rowArea.max.x - 4.0f;
        dl->AddLine(ImVec2(lineStartX, lineY), ImVec2(lineEndX, lineY), IM_COL32(0x4a, 0x4a, 0x4a, 0xff), 2.0f);
    }
}

void TimelinePanel::drawVideoClip(const Clip& c, Rect rowArea) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float x1 = rowArea.min.x + 60.0f + static_cast<float>(c.startTick) * mPixelsPerTick;
    float x2 = rowArea.min.x + 60.0f + static_cast<float>(c.endTick) * mPixelsPerTick;

    ImRect clipRect(ImVec2(x1, rowArea.min.y + 2), ImVec2(x2, rowArea.max.y - 2));
    dl->AddRectFilled(clipRect.Min, clipRect.Max, IM_COL32(0x2a, 0x5a, 0x8a, 0xcc), 4.0f);
    dl->AddRect(clipRect.Min, clipRect.Max, IM_COL32(0x4a, 0x8a, 0xba, 0xff), 4.0f);

    // Clip name
    ImGui::SetCursorScreenPos(ImVec2(x1 + 4, rowArea.min.y + 4));
    ImGui::Text("%s", c.name.c_str());
}

void TimelinePanel::drawKeyframe(const CameraKeyframe& k, Rect rowArea) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float x = rowArea.min.x + 80.0f + static_cast<float>(k.tick) * mPixelsPerTick;
    float y = (rowArea.min.y + rowArea.max.y) * 0.5f;

    bool selected = false;
    auto* sel = Editor::getInstance().selection().getAs<SelectedKeyframe>();
    if (sel && sel->trackId == k.id) selected = true;

    ImU32 color = selected ? IM_COL32(0xf0, 0xc0, 0x20, 0xff) : IM_COL32(0x80, 0xc0, 0xf0, 0xff);
    dl->AddCircleFilled(ImVec2(x, y), 4.0f, color);
    if (selected) {
        dl->AddCircle(ImVec2(x, y), 6.0f, IM_COL32(0xf0, 0xc0, 0x20, 0xff), 0, 2.0f);
    }
}

void TimelinePanel::drawMarker(const Marker& m, Rect rowArea) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float x = rowArea.min.x + 60.0f + static_cast<float>(m.tick) * mPixelsPerTick;
    float y1 = rowArea.min.y + 2;
    float y2 = rowArea.max.y - 2;

    dl->AddLine(ImVec2(x, y1), ImVec2(x, y2), IM_COL32(0xf0, 0xc0, 0x20, 0xaa), 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(x + 4, rowArea.min.y + 1));
    ImGui::Text("%s %s", ICON_MARKER, m.label.c_str());
}

void TimelinePanel::drawTransition(const Transition& t, Rect area) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float x = area.min.x + 60.0f + static_cast<float>(t.startTick) * mPixelsPerTick;
    float w = static_cast<float>(t.durationTicks) * mPixelsPerTick;

    ImRect transRect(ImVec2(x, area.min.y + 4), ImVec2(x + w, area.max.y - 4));
    dl->AddRectFilled(transRect.Min, transRect.Max, IM_COL32(0x3a, 0x8c, 0xf0, 0x66), 4.0f);
    dl->AddRect(transRect.Min, transRect.Max, IM_COL32(0x3a, 0x8c, 0xf0, 0xaa), 4.0f);

    ImGui::SetCursorScreenPos(ImVec2(x + 4, area.min.y + 4));
    ImGui::Text("%s %s", ICON_TRANSITION, t.type.c_str());
}

void TimelinePanel::handleScrubDrag(Rect headerArea) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    if (!headerArea.contains(ImGui::GetMousePos())) return;

    float mouseX = ImGui::GetMousePos().x - headerArea.min.x - 60.0f;
    int tick = static_cast<int>(mouseX / mPixelsPerTick);
    tick = std::clamp(tick, 0, Editor::getInstance().state().totalTicks);

    if (mPlayheadTick != tick) {
        mPlayheadTick = tick;
        Editor::getInstance().state().currentTick = tick;
    }
}

void TimelinePanel::handleClipDrag(Clip& c, Rect rowArea) {
    if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left)) return;
    if (mDragTargetId != c.id) return;

    float mouseX = ImGui::GetMousePos().x - rowArea.min.x - 60.0f;
    int newTick = static_cast<int>(mouseX / mPixelsPerTick);
    int duration = c.endTick - c.startTick;
    c.startTick = std::clamp(newTick, 0, Editor::getInstance().state().totalTicks - duration);
    c.endTick = c.startTick + duration;
}

void TimelinePanel::handleKeyframeDrag(CameraKeyframe& k, Rect rowArea) {
    if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left)) return;
    if (mDragTargetId != k.id) return;

    float mouseX = ImGui::GetMousePos().x - rowArea.min.x - 80.0f;
    int newTick = static_cast<int>(mouseX / mPixelsPerTick);

    // Snap: Shift=no snap, Ctrl=0.5s grid, Alt=1s grid
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl) {
        newTick = (newTick / 10) * 10;  // 0.5s grid (20t/s * 0.5s = 10t)
    } else if (io.KeyAlt) {
        newTick = (newTick / 20) * 20;  // 1s grid
    }

    k.tick = std::clamp(newTick, 0, Editor::getInstance().state().totalTicks);
}

void TimelinePanel::onWheel(float deltaY) {
    if (ImGui::IsKeyDown(ImGuiKey_LeftShift)) {
        mPixelsPerTick *= (deltaY > 0 ? 1.1f : 1.0f / 1.1f);
        mPixelsPerTick = std::clamp(mPixelsPerTick, 0.02f, 2.0f);
    } else {
        mScrollX -= deltaY * 20.0f;
        mScrollX = std::max(0.0f, mScrollX);
    }
}

} // namespace playback::refactor::editor