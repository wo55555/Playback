#include "TimelinePanel.h"

#include "playback/refactor/editor/Editor.h"
#include "playback/refactor/editor/EditorBridge.h"
#include "playback/refactor/editor/HintBar.h"
#include "playback/refactor/editor/iconfont.h"
#include "playback/refactor/editor/models/SelectionModel.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>

namespace playback::refactor::editor {

// ──────────────────────────────────────────────────────────────
//  draw() — main entry
// ──────────────────────────────────────────────────────────────

void TimelinePanel::draw() {
    auto& state = Editor::getInstance().state();

    // ── Sync playhead position from bridge state every frame ──
    mPlayheadTick = state.currentTick;

    ImGui::Begin("Timeline", nullptr,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    drawHeader();
    ImGui::Separator();
    drawRuler();
    drawBody();

    // Hint bar at bottom
    ImGui::Separator();
    HintBar hintBar;
    hintBar.draw();

    // Mouse wheel zoom
    if (ImGui::IsWindowHovered()) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f) {
            onWheel(io.MouseWheel);
        }
    }

    ImGui::End();

    // ── End-of-frame: commit drag operation through bridge ──
    if (mDragType != DragType::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // Commit the drag operation to the command stack
        commitDragOperation();
        mDragType = DragType::None;
        mDragTargetId.clear();
    }
}

// ──────────────────────────────────────────────────────────────
//  drawHeader() — play/pause, timecode, buttons
// ──────────────────────────────────────────────────────────────

void TimelinePanel::drawHeader() {
    auto& state = Editor::getInstance().state();

    // Play / Pause
    if (ImGui::Button(state.playing ? ICON_PAUSE : ICON_PLAY)) {
        EditorBridge::getInstance().playPause();
    }
    ImGui::SameLine();

    // Timecode
    int totalSeconds = state.currentTick / 20;
    int hours   = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int secs    = totalSeconds % 60;
    int millis  = (state.currentTick % 20) * 50;

    int totalDuration = state.totalTicks / 20;
    int durHours   = totalDuration / 3600;
    int durMinutes = (totalDuration % 3600) / 60;
    int durSecs    = totalDuration % 60;
    int durMillis  = (state.totalTicks % 20) * 50;

    char timecode[64];
    std::snprintf(timecode, sizeof(timecode),
        "%02d:%02d:%02d.%03d / %02d:%02d:%02d.%03d",
        hours, minutes, secs, millis,
        durHours, durMinutes, durSecs, durMillis);
    ImGui::TextUnformatted(timecode);
    ImGui::SameLine();

    // Add keyframe
    if (ImGui::Button(ICON_ADD_KEYFRAME)) {
        // Add keyframe on the active camera track at playhead position
        for (auto& track : state.cameraTracks) {
            if (track.active) {
                EditorBridge::getInstance().addKeyframe(state, track.id, state.currentTick);
                break;
            }
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Add Keyframe at Playhead (K)");
    }
    ImGui::SameLine();

    // Add marker
    if (ImGui::Button(ICON_ADD_MARKER)) {
        EditorBridge::getInstance().addMarker(state, "Marker", state.currentTick);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Add Marker at Playhead (M)");
    }
    ImGui::SameLine();

    // Split
    if (ImGui::Button(ICON_SPLIT)) {
        handleSplitAtPlayhead();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Split Clip at Playhead (Ctrl+K)");
    }
    ImGui::SameLine();

    // Delete
    if (ImGui::Button(ICON_DELETE)) {
        if (!mSelectedClipId.empty()) {
            handleRippleDelete();
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Delete Selected Clip (Del)");
    }
    ImGui::SameLine();

    // Undo / Redo
    if (ImGui::Button(ICON_UNDO)) {
        EditorBridge::getInstance().undo(state);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_REDO)) {
        EditorBridge::getInstance().redo(state);
    }
    ImGui::SameLine();

    // Zoom controls
    ImGui::Text("Zoom:");
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
}

// ──────────────────────────────────────────────────────────────
//  drawRuler() — tick marks + scrub
// ──────────────────────────────────────────────────────────────

void TimelinePanel::drawRuler() {
    auto& state = Editor::getInstance().state();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float availW = ImGui::GetContentRegionAvail().x;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    Rect rulerArea;
    rulerArea.min = pos;
    rulerArea.max = ImVec2(pos.x + availW, pos.y + kRulerHeight);

    // Background
    dl->AddRectFilled(rulerArea.min, rulerArea.max, IM_COL32(0x14, 0x14, 0x14, 0xff));
    dl->AddRect(rulerArea.min, rulerArea.max, IM_COL32(0x3a, 0x3a, 0x3a, 0xff));

    // Ticks
    float contentStartX = rulerArea.min.x + kLabelWidth;
    float contentEndX   = rulerArea.max.x;
    int totalTicks = state.totalTicks;
    if (totalTicks > 0) {
        // Determine tick interval based on zoom
        float pxPerTick = mPixelsPerTick;
        int step = 1;
        if (pxPerTick * 5 < 10.0f)  step = 20;  // 1s
        if (pxPerTick * 20 < 10.0f) step = 100; // 5s
        if (pxPerTick * 100 < 10.0f) step = 200; // 10s

        for (int t = 0; t <= totalTicks; t += step) {
            float x = contentStartX + static_cast<float>(t) * pxPerTick;
            if (x < contentStartX || x > contentEndX) continue;

            bool major = (t % (step * 5) == 0);
            float tickH = major ? 12.0f : 6.0f;
            dl->AddLine(ImVec2(x, rulerArea.max.y - tickH), ImVec2(x, rulerArea.max.y),
                IM_COL32(0x5a, 0x5a, 0x5a, 0xff));

            if (major) {
                int sec = t / 20;
                int m = sec / 60;
                int s = sec % 60;
                char label[16];
                std::snprintf(label, sizeof(label), "%d:%02d", m, s);
                dl->AddText(ImVec2(x + 2, rulerArea.max.y - 22),
                    IM_COL32(0x8a, 0x8a, 0x8a, 0xff), label);
            }
        }
    }

    // Draw playhead line on ruler
    float phX = contentStartX + static_cast<float>(mPlayheadTick) * mPixelsPerTick;
    if (phX >= contentStartX && phX <= contentEndX) {
        dl->AddLine(ImVec2(phX, rulerArea.min.y), ImVec2(phX, rulerArea.max.y),
            IM_COL32(0xf0, 0xc0, 0x20, 0xff), 2.0f);
        // Triangle at top of ruler
        dl->AddTriangleFilled(
            ImVec2(phX - 4, rulerArea.min.y + 18),
            ImVec2(phX + 4, rulerArea.min.y + 18),
            ImVec2(phX, rulerArea.min.y + 10),
            IM_COL32(0xf0, 0xc0, 0x20, 0xff));
    }

    // Handle ruler click
    handleRulerClick(rulerArea);

    ImGui::SetCursorScreenPos(ImVec2(rulerArea.min.x, rulerArea.max.y));
}

// ──────────────────────────────────────────────────────────────
//  drawBody() — all tracks
// ──────────────────────────────────────────────────────────────

void TimelinePanel::drawBody() {
    auto& state = Editor::getInstance().state();
    auto& editor = Editor::getInstance();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float bodyWidth = ImGui::GetContentRegionAvail().x;
    float contentStartX = ImGui::GetCursorScreenPos().x + kLabelWidth;

    // ── Video tracks ──
    for (int vi = 0; vi < static_cast<int>(state.videoTracks.size()); ++vi) {
        auto& vt = state.videoTracks[vi];
        if (!vt.visible) continue;

        float trackH = static_cast<float>(vt.height);
        Rect rowArea;
        rowArea.min = ImGui::GetCursorScreenPos();
        rowArea.max = ImVec2(rowArea.min.x + bodyWidth, rowArea.min.y + trackH);

        drawVideoTrack(vi, rowArea);
        ImGui::SetCursorScreenPos(ImVec2(rowArea.min.x, rowArea.max.y));
    }

    // ── Camera tracks ──
    drawCameraTrackRows();

    // ── Marker track ──
    drawMarkerRow();

    // ── Playhead line (full height) ──
    {
        Rect fullArea;
        fullArea.min = ImGui::GetCursorScreenPos();
        fullArea.min.x = ImGui::GetWindowPos().x + kLabelWidth;
        fullArea.max = ImGui::GetWindowContentRegionMax();
        fullArea.max.x += ImGui::GetWindowPos().x;
        // Estimate full height from viewport
        fullArea.max.y = fullArea.min.y + ImGui::GetContentRegionAvail().y;
        drawPlayhead(fullArea);
    }

    // Clear drag if mouse released
    if (mDragType != DragType::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        mDragType = DragType::None;
        mDragTargetId.clear();
    }
}

// ──────────────────────────────────────────────────────────────
//  drawVideoTrack() — single video row
// ──────────────────────────────────────────────────────────────

void TimelinePanel::drawVideoTrack(int trackIndex, Rect rowArea) {
    auto& vt = Editor::getInstance().state().videoTracks[trackIndex];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float contentStartX = rowArea.min.x + kLabelWidth;

    // Background
    bool isActive = (trackIndex == mSelectedTrackIndex);
    ImU32 bg = isActive
        ? IM_COL32(0x22, 0x22, 0x28, 0xff)
        : IM_COL32(0x1e, 0x1e, 0x1e, 0xff);
    dl->AddRectFilled(rowArea.min, rowArea.max, bg);
    dl->AddRect(rowArea.min, rowArea.max, IM_COL32(0x3a, 0x3a, 0x3a, 0xff));

    // Track label area
    ImGui::SetCursorScreenPos(ImVec2(rowArea.min.x + 2, rowArea.min.y + 2));
    ImGui::BeginGroup();
    ImGui::Text("%s", vt.name.c_str());
    if (vt.locked) {
        ImGui::SameLine();
        ImGui::TextDisabled(ICON_LOCK);
    }
    ImGui::EndGroup();

    // Right-click on track label area → track context menu
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        mSelectedTrackIndex = trackIndex;
        ImGui::OpenPopup("TrackContextMenu");
    }
    drawTrackContextMenu(trackIndex);

    // ── Draw transitions (under clips) ──
    for (const auto& t : Editor::getInstance().state().transitions) {
        drawTransitionBetween(trackIndex, t, rowArea);
    }

    // ── Draw clips ──
    for (int ci = 0; ci < static_cast<int>(vt.clips.size()); ++ci) {
        drawVideoClipOnTrack(vt.clips[ci], rowArea, trackIndex);
    }

    // ── Track context menu (right-click on empty area) ──
    // Push invisible button to cover the track area for right-click
    Rect clickArea;
    clickArea.min = ImVec2(contentStartX, rowArea.min.y);
    clickArea.max = rowArea.max;
    ImGui::SetCursorScreenPos(clickArea.min);
    ImGui::InvisibleButton(("##track_" + vt.id).c_str(),
        ImVec2(clickArea.max.x - clickArea.min.x, clickArea.max.y - clickArea.min.y));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        mSelectedTrackIndex = trackIndex;
        ImGui::OpenPopup("TrackContextMenu");
    }
}

// ──────────────────────────────────────────────────────────────
//  drawVideoClipOnTrack() — single clip rect + interactions
// ──────────────────────────────────────────────────────────────

void TimelinePanel::drawVideoClipOnTrack(const Clip& c, Rect rowArea, int trackIndex) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float contentStartX = rowArea.min.x + kLabelWidth;

    int clipEnd = c.trackTick + (c.outTick - c.inTick);
    float x1 = contentStartX + static_cast<float>(c.trackTick) * mPixelsPerTick;
    float x2 = contentStartX + static_cast<float>(clipEnd) * mPixelsPerTick;

    // Clamp to visible area
    if (x2 < contentStartX || x1 > rowArea.max.x) return;

    bool isSelected = (c.id == mSelectedClipId);
    ImRect clipRect(ImVec2(x1, rowArea.min.y + 2), ImVec2(x2, rowArea.max.y - 2));

    // Clip body
    ImU32 fillColor = IM_COL32(0x2a, 0x5a, 0x8a, 0xcc);
    ImU32 borderColor = isSelected ? IM_COL32(0xf0, 0xc0, 0x20, 0xff) : IM_COL32(0x4a, 0x8a, 0xba, 0xff);
    dl->AddRectFilled(clipRect.Min, clipRect.Max, fillColor, 4.0f);
    dl->AddRect(clipRect.Min, clipRect.Max, borderColor, 4.0f);

    // Clip name
    ImGui::SetCursorScreenPos(ImVec2(x1 + 4, rowArea.min.y + 3));
    ImGui::Text("%s", c.name.c_str());

    // Muted indicator
    if (c.muted) {
        ImGui::SameLine();
        ImGui::TextDisabled(ICON_MUTE);
    }

    // Locked indicator
    if (c.locked) {
        ImGui::SameLine();
        ImGui::TextDisabled(ICON_LOCK);
    }

    // Duration label (right-aligned in clip)
    char durLabel[32];
    std::snprintf(durLabel, sizeof(durLabel), "+%d", c.outTick - c.inTick);
    ImVec2 durSize = ImGui::CalcTextSize(durLabel);
    dl->AddText(ImVec2(x2 - durSize.x - 4, rowArea.min.y + 3),
        IM_COL32(0xaa, 0xaa, 0xaa, 0xaa), durLabel);

    // ── Hit test & interaction ──
    ImVec2 mouse = ImGui::GetMousePos();
    bool hovered = clipRect.Contains(mouse);

    // Edge hit: left/right 6px
    bool onLeftEdge  = (std::abs(mouse.x - x1) < 6.0f) && (mouse.y >= clipRect.Min.y && mouse.y <= clipRect.Max.y);
    bool onRightEdge = (std::abs(mouse.x - x2) < 6.0f) && (mouse.y >= clipRect.Min.y && mouse.y <= clipRect.Max.y);

    // Cursor change
    if (onLeftEdge || onRightEdge) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    // Click
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        mSelectedClipId = c.id;
        mSelectedTrackIndex = trackIndex;

        if (onLeftEdge) {
            mDragType = DragType::TrimClipIn;
            mDragTargetId = c.id;
            mDragTrackIndex = trackIndex;
            mDragStartTick = c.trackTick;
            mDragOrigTick = c.inTick;
        } else if (onRightEdge) {
            mDragType = DragType::TrimClipOut;
            mDragTargetId = c.id;
            mDragTrackIndex = trackIndex;
            mDragStartTick = clipEnd;
            mDragOrigTick = c.outTick;
        } else {
            mDragType = DragType::MoveClip;
            mDragTargetId = c.id;
            mDragTrackIndex = trackIndex;
            mDragStartTick = c.trackTick;
        }
    }

    // Right-click
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        mSelectedClipId = c.id;
        mSelectedTrackIndex = trackIndex;
        ImGui::OpenPopup("ClipContextMenu");
    }
    drawClipContextMenu(c, trackIndex);

    // Drag update
    if (mDragType != DragType::None && mDragTargetId == c.id) {
        // Find mutable ref — need to access through Editor state
        auto& vt = Editor::getInstance().state().videoTracks[trackIndex];
        for (auto& clip : vt.clips) {
            if (clip.id == c.id) {
                // Calculate new position
                float mouseX = ImGui::GetMousePos().x;
                int newTick = static_cast<int>((mouseX - contentStartX) / mPixelsPerTick);
                newTick = std::max(0, newTick);

                switch (mDragType) {
                    case DragType::MoveClip: {
                        int delta = newTick - mDragStartTick;
                        clip.trackTick = std::max(0, mDragStartTick + delta);
                        clip.trackTick = std::min(clip.trackTick,
                            Editor::getInstance().state().totalTicks - (clip.outTick - clip.inTick));
                        break;
                    }
                    case DragType::TrimClipIn: {
                        int delta = newTick - mDragStartTick;
                        int newIn = mDragOrigTick + delta;
                        int minIn = 0;
                        int maxIn = clip.outTick - 1;
                        clip.inTick = std::clamp(newIn, minIn, maxIn);
                        clip.trackTick = mDragStartTick + (clip.inTick - mDragOrigTick);
                        break;
                    }
                    case DragType::TrimClipOut: {
                        int delta = newTick - mDragStartTick;
                        int newOut = mDragOrigTick + delta;
                        int minOut = clip.inTick + 1;
                        int maxOut = clip.inTick + 10000; // reasonable max
                        clip.outTick = std::clamp(newOut, minOut, maxOut);
                        break;
                    }
                    default: break;
                }
                break;
            }
        }
    }
}

// ──────────────────────────────────────────────────────────────
//  drawTransitionBetween() — transition overlay on video track
// ──────────────────────────────────────────────────────────────

void TimelinePanel::drawTransitionBetween(int trackIndex, const Transition& t, Rect rowArea) {
    auto& state = Editor::getInstance().state();
    if (trackIndex >= static_cast<int>(state.videoTracks.size())) return;
    auto& vt = state.videoTracks[trackIndex];

    // Find the two clips
    const Clip* clipA = nullptr;
    const Clip* clipB = nullptr;
    for (const auto& c : vt.clips) {
        if (c.id == t.fromClipId) clipA = &c;
        if (c.id == t.toClipId)   clipB = &c;
    }
    if (!clipA || !clipB) return;

    float contentStartX = rowArea.min.x + kLabelWidth;
    int transStart = clipB->trackTick - t.durationTicks;
    int transEnd   = clipB->trackTick;

    float x1 = contentStartX + static_cast<float>(transStart) * mPixelsPerTick;
    float x2 = contentStartX + static_cast<float>(transEnd) * mPixelsPerTick;

    if (x2 < contentStartX || x1 > rowArea.max.x) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImRect transRect(ImVec2(x1, rowArea.min.y + 2), ImVec2(x2, rowArea.max.y - 2));
    dl->AddRectFilled(transRect.Min, transRect.Max, IM_COL32(0x3a, 0x8c, 0xf0, 0x55), 4.0f);
    dl->AddRect(transRect.Min, transRect.Max, IM_COL32(0x3a, 0x8c, 0xf0, 0x88), 4.0f);

    const char* kindNames[] = {"Cut", "Fade", "CrossDissolve"};
    int idx = static_cast<int>(t.kind);
    const char* kindName = (idx >= 0 && idx < 3) ? kindNames[idx] : "?";

    ImGui::SetCursorScreenPos(ImVec2(x1 + 4, rowArea.min.y + 3));
    ImGui::Text("%s %s", ICON_TRANSITION, kindName);
}

// ──────────────────────────────────────────────────────────────
//  drawCameraTrackRows()
// ──────────────────────────────────────────────────────────────

void TimelinePanel::drawCameraTrackRows() {
    auto& state = Editor::getInstance().state();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float bodyWidth = ImGui::GetContentRegionAvail().x;
    float contentStartX = ImGui::GetCursorScreenPos().x + kLabelWidth;

    for (auto& track : state.cameraTracks) {
        Rect rowArea;
        rowArea.min = ImGui::GetCursorScreenPos();
        rowArea.max = ImVec2(rowArea.min.x + bodyWidth, rowArea.min.y + kCameraTrackH);

        // Background
        dl->AddRectFilled(rowArea.min, rowArea.max,
            track.active ? IM_COL32(0x22, 0x22, 0x28, 0xff) : IM_COL32(0x1a, 0x1a, 0x1a, 0xff));
        dl->AddRect(rowArea.min, rowArea.max, IM_COL32(0x3a, 0x3a, 0x3a, 0xff));

        // Label
        ImGui::SetCursorScreenPos(ImVec2(rowArea.min.x + 4, rowArea.min.y + 2));
        ImGui::Text("%s %s",
            track.active ? ICON_TRACK_ACTIVE : ICON_TRACK_OFF,
            track.name.c_str());

        // Active camera track progress line
        if (track.active) {
            float lineY = (rowArea.min.y + rowArea.max.y) * 0.5f;
            float lineStartX = contentStartX;
            float lineEndX   = rowArea.max.x - 4.0f;
            dl->AddLine(ImVec2(lineStartX, lineY), ImVec2(lineEndX, lineY),
                IM_COL32(0x4a, 0x4a, 0x4a, 0xff), 2.0f);
        }

        // Keyframes
        for (auto& kf : track.keyframes) {
            drawKeyframeOnTrack(kf, rowArea);
        }

        ImGui::SetCursorScreenPos(ImVec2(rowArea.min.x, rowArea.max.y));
    }
}

// ──────────────────────────────────────────────────────────────
//  drawKeyframeOnTrack()
// ──────────────────────────────────────────────────────────────

void TimelinePanel::drawKeyframeOnTrack(const CameraKeyframe& k, Rect rowArea) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float contentStartX = rowArea.min.x + kLabelWidth;

    float x = contentStartX + static_cast<float>(k.tick) * mPixelsPerTick;
    float y = (rowArea.min.y + rowArea.max.y) * 0.5f;

    // Check if selected
    bool selected = false;
    auto* sel = Editor::getInstance().selection().getAs<SelectedKeyframe>();
    if (sel && sel->trackId == k.id) selected = true;

    ImU32 color = selected
        ? IM_COL32(0xf0, 0xc0, 0x20, 0xff)
        : IM_COL32(0x80, 0xc0, 0xf0, 0xff);

    dl->AddCircleFilled(ImVec2(x, y), 4.0f, color);
    if (selected) {
        dl->AddCircle(ImVec2(x, y), 6.0f, IM_COL32(0xf0, 0xc0, 0x20, 0xff), 0, 2.0f);
    }

    // Click
    ImVec2 mouse = ImGui::GetMousePos();
    float dist = std::sqrt((mouse.x - x) * (mouse.x - x) + (mouse.y - y) * (mouse.y - y));
    bool hovered = (dist < 6.0f);

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        mDragType = DragType::MoveKeyframe;
        mDragTargetId = k.id;
        mDragStartTick = k.tick;
    }

    // Drag
    if (mDragType == DragType::MoveKeyframe && mDragTargetId == k.id) {
        float mouseX = ImGui::GetMousePos().x;
        int newTick = static_cast<int>((mouseX - contentStartX) / mPixelsPerTick);

        // Snap
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl) {
            newTick = (newTick / 10) * 10;
        } else if (io.KeyAlt) {
            newTick = (newTick / 20) * 20;
        }

        // Need mutable ref — search through state
        for (auto& track : Editor::getInstance().state().cameraTracks) {
            for (auto& kf : track.keyframes) {
                if (kf.id == k.id) {
                    kf.tick = std::clamp(newTick, 0,
                        Editor::getInstance().state().totalTicks);
                    break;
                }
            }
        }
    }
}

// ──────────────────────────────────────────────────────────────
//  drawMarkerRow()
// ──────────────────────────────────────────────────────────────

void TimelinePanel::drawMarkerRow() {
    auto& state = Editor::getInstance().state();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float bodyWidth = ImGui::GetContentRegionAvail().x;
    float contentStartX = ImGui::GetCursorScreenPos().x + kLabelWidth;

    Rect rowArea;
    rowArea.min = ImGui::GetCursorScreenPos();
    rowArea.max = ImVec2(rowArea.min.x + bodyWidth, rowArea.min.y + kMarkerTrackH);

    dl->AddRectFilled(rowArea.min, rowArea.max, IM_COL32(0x18, 0x18, 0x18, 0xff));
    dl->AddRect(rowArea.min, rowArea.max, IM_COL32(0x3a, 0x3a, 0x3a, 0xff));

    ImGui::SetCursorScreenPos(rowArea.min);
    ImGui::Text("M");

    for (auto& marker : state.markers) {
        drawMarkerOnRow(marker, rowArea);
    }

    ImGui::SetCursorScreenPos(ImVec2(rowArea.min.x, rowArea.max.y));
}

// ──────────────────────────────────────────────────────────────
//  drawMarkerOnRow()
// ──────────────────────────────────────────────────────────────

void TimelinePanel::drawMarkerOnRow(const Marker& m, Rect rowArea) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float contentStartX = rowArea.min.x + kLabelWidth;

    float x = contentStartX + static_cast<float>(m.tick) * mPixelsPerTick;
    float y1 = rowArea.min.y + 2;
    float y2 = rowArea.max.y - 2;

    dl->AddLine(ImVec2(x, y1), ImVec2(x, y2), IM_COL32(0xf0, 0xc0, 0x20, 0xaa), 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(x + 4, rowArea.min.y + 1));
    ImGui::Text("%s %s", ICON_MARKER, m.label.c_str());
}

// ──────────────────────────────────────────────────────────────
//  drawPlayhead() — vertical yellow line across all tracks
// ──────────────────────────────────────────────────────────────

void TimelinePanel::drawPlayhead(Rect fullArea) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float contentStartX = fullArea.min.x;
    float phX = contentStartX + static_cast<float>(mPlayheadTick) * mPixelsPerTick;

    if (phX >= contentStartX && phX <= fullArea.max.x) {
        dl->AddLine(ImVec2(phX, fullArea.min.y), ImVec2(phX, fullArea.max.y),
            IM_COL32(0xf0, 0xc0, 0x20, 0x88), 1.0f);
    }
}

// ──────────────────────────────────────────────────────────────
//  Interaction helpers
// ──────────────────────────────────────────────────────────────

void TimelinePanel::handleRulerClick(Rect rulerArea) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    if (!rulerArea.contains(ImGui::GetMousePos())) return;

    float contentStartX = rulerArea.min.x + kLabelWidth;
    float mouseX = ImGui::GetMousePos().x - contentStartX;
    int tick = static_cast<int>(mouseX / mPixelsPerTick);
    tick = std::clamp(tick, 0, Editor::getInstance().state().totalTicks);

    if (mPlayheadTick != tick) {
        mPlayheadTick = tick;
        Editor::getInstance().state().currentTick = tick;
        // Commit seek through bridge
        EditorBridge::getInstance().seek(tick);
    }
}

void TimelinePanel::handleClipClick(const Clip& c, Rect rowArea, int trackIndex, bool hovered) {
    // Handled inline in drawVideoClipOnTrack
    (void)c; (void)rowArea; (void)trackIndex; (void)hovered;
}

void TimelinePanel::handleKeyframeClick(const CameraKeyframe& k, Rect rowArea, bool hovered) {
    // Handled inline in drawKeyframeOnTrack
    (void)k; (void)rowArea; (void)hovered;
}

void TimelinePanel::clipDragUpdate(Clip& c, Rect rowArea) {
    // Handled inline in drawVideoClipOnTrack
    (void)c; (void)rowArea;
}

void TimelinePanel::keyframeDragUpdate(CameraKeyframe& k, Rect rowArea) {
    // Handled inline in drawKeyframeOnTrack
    (void)k; (void)rowArea;
}

// ──────────────────────────────────────────────────────────────
//  Actions
// ──────────────────────────────────────────────────────────────

void TimelinePanel::handleSplitAtPlayhead() {
    if (mSelectedClipId.empty() || mSelectedTrackIndex < 0) return;

    auto& state = Editor::getInstance().state();
    if (mSelectedTrackIndex >= static_cast<int>(state.videoTracks.size())) return;

    auto& vt = state.videoTracks[mSelectedTrackIndex];
    for (int ci = 0; ci < static_cast<int>(vt.clips.size()); ++ci) {
        auto& c = vt.clips[ci];
        if (c.id != mSelectedClipId) continue;

        int clipEnd = c.trackTick + (c.outTick - c.inTick);
        if (mPlayheadTick <= c.trackTick || mPlayheadTick >= clipEnd) break;

        EditorBridge::getInstance().splitClip(
            state, vt.id, mSelectedClipId, mPlayheadTick);
        break;
    }
}

void TimelinePanel::handleRippleDelete() {
    if (mSelectedClipId.empty() || mSelectedTrackIndex < 0) return;

    EditorBridge::getInstance().deleteClip(
        Editor::getInstance().state(),
        Editor::getInstance().state().videoTracks[mSelectedTrackIndex].id,
        mSelectedClipId);
    mSelectedClipId.clear();
}

void TimelinePanel::handleCopy() {
    // Placeholder — clipboard interface from 04
}

void TimelinePanel::handlePaste() {
    // Placeholder — clipboard interface from 04
}

void TimelinePanel::handleTrimLeftToPlayhead() {
    if (mSelectedClipId.empty() || mSelectedTrackIndex < 0) return;
    auto& state = Editor::getInstance().state();
    if (mSelectedTrackIndex >= static_cast<int>(state.videoTracks.size())) return;

    auto& vt = state.videoTracks[mSelectedTrackIndex];
    for (auto& c : vt.clips) {
        if (c.id != mSelectedClipId) continue;
        int localTick = mPlayheadTick - c.trackTick;
        if (localTick > 0 && localTick < (c.outTick - c.inTick)) {
            int newIn = c.inTick + localTick;
            int newTrackTick = mPlayheadTick;
            // Submit via bridge for undo/redo
            EditorBridge::getInstance().trimClip(state, vt.id, mSelectedClipId, newIn, c.outTick);
        }
        break;
    }
}

void TimelinePanel::handleTrimRightToPlayhead() {
    if (mSelectedClipId.empty() || mSelectedTrackIndex < 0) return;
    auto& state = Editor::getInstance().state();
    if (mSelectedTrackIndex >= static_cast<int>(state.videoTracks.size())) return;

    auto& vt = state.videoTracks[mSelectedTrackIndex];
    for (auto& c : vt.clips) {
        if (c.id != mSelectedClipId) continue;
        int localTick = mPlayheadTick - c.trackTick;
        if (localTick > 0 && localTick < (c.outTick - c.inTick)) {
            // Save pre-drag state, then submit via bridge
            int oldOut = c.outTick;
            c.outTick = c.inTick + localTick;
            // Submit command for undo/redo
            EditorBridge::getInstance().trimClip(
                Editor::getInstance().state(), vt.id, mSelectedClipId, c.inTick, c.outTick);
        }
        break;
    }
}

// ──────────────────────────────────────────────────────────────
//  commitDragOperation() — commit drag to command stack
// ──────────────────────────────────────────────────────────────

void TimelinePanel::commitDragOperation() {
    if (mDragTargetId.empty() || mDragTrackIndex < 0) return;
    auto& state = Editor::getInstance().state();
    if (mDragTrackIndex >= static_cast<int>(state.videoTracks.size())) return;

    auto& vt = state.videoTracks[mDragTrackIndex];

    switch (mDragType) {
        case DragType::MoveClip: {
            // Find the clip and get its current (post-drag) trackTick
            for (auto& clip : vt.clips) {
                if (clip.id != mDragTargetId) continue;
                int oldTrackTick = mDragStartTick;
                int newTrackTick = clip.trackTick;
                if (oldTrackTick == newTrackTick) break; // no change

                // Restore pre-drag state, then submit via bridge
                clip.trackTick = oldTrackTick;
                EditorBridge::getInstance().moveClip(state, vt.id, mDragTargetId, newTrackTick);
                break;
            }
            break;
        }
        case DragType::TrimClipIn: {
            for (auto& clip : vt.clips) {
                if (clip.id != mDragTargetId) continue;
                int oldIn = mDragOrigTick;
                int oldTrackTick = mDragStartTick;
                int newIn = clip.inTick;
                int newTrackTick = clip.trackTick;
                if (oldIn == newIn) break;

                // Restore pre-drag state
                clip.inTick = oldIn;
                clip.trackTick = oldTrackTick;
                EditorBridge::getInstance().trimClip(state, vt.id, mDragTargetId, newIn, clip.outTick);
                break;
            }
            break;
        }
        case DragType::TrimClipOut: {
            for (auto& clip : vt.clips) {
                if (clip.id != mDragTargetId) continue;
                int oldOut = mDragOrigTick;
                int newOut = clip.outTick;
                if (oldOut == newOut) break;

                // Restore pre-drag state
                clip.outTick = oldOut;
                EditorBridge::getInstance().trimClip(state, vt.id, mDragTargetId, clip.inTick, newOut);
                break;
            }
            break;
        }
        case DragType::MoveKeyframe: {
            // Find the keyframe and commit via bridge
            for (auto& track : state.cameraTracks) {
                for (auto& kf : track.keyframes) {
                    if (kf.id != mDragTargetId) continue;
                    int oldTick = mDragStartTick;
                    int newTick = kf.tick;
                    if (oldTick == newTick) break;

                    // Restore, then submit
                    kf.tick = oldTick;
                    EditorBridge::getInstance().moveKeyframe(state, track.id, mDragTargetId, newTick);
                    break;
                }
            }
            break;
        }
        default:
            break;
    }
}

// ──────────────────────────────────────────────────────────────
//  Context menus
// ──────────────────────────────────────────────────────────────

void TimelinePanel::drawClipContextMenu(const Clip& c, int trackIndex) {
    if (ImGui::BeginPopup("ClipContextMenu")) {
        if (ImGui::MenuItem("Split at Playhead", "Ctrl+K")) {
            handleSplitAtPlayhead();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Trim Left to Playhead")) {
            handleTrimLeftToPlayhead();
        }
        if (ImGui::MenuItem("Trim Right to Playhead")) {
            handleTrimRightToPlayhead();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Copy", "Ctrl+C")) {
            handleCopy();
        }
        if (ImGui::MenuItem("Paste", "Ctrl+V")) {
            handlePaste();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Ripple Delete", "Del")) {
            handleRippleDelete();
        }
        ImGui::EndPopup();
    }
}

void TimelinePanel::drawTrackContextMenu(int trackIndex) {
    if (ImGui::BeginPopup("TrackContextMenu")) {
        auto& state = Editor::getInstance().state();
        if (trackIndex >= 0 && trackIndex < static_cast<int>(state.videoTracks.size())) {
            auto& vt = state.videoTracks[trackIndex];
            if (ImGui::MenuItem("Rename Track")) {
                // Placeholder
            }
            if (ImGui::MenuItem(vt.locked ? "Unlock Track" : "Lock Track")) {
                vt.locked = !vt.locked;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Track")) {
                state.videoTracks.erase(state.videoTracks.begin() + trackIndex);
                mSelectedTrackIndex = -1;
            }
        }
        ImGui::EndPopup();
    }
}

// ──────────────────────────────────────────────────────────────
//  Hit testing
// ──────────────────────────────────────────────────────────────

std::optional<TimelinePanel::ClipHit> TimelinePanel::hitTestClip(
    ImVec2 mousePos, Rect rowArea, int trackIndex)
{
    (void)rowArea;
    auto& vt = Editor::getInstance().state().videoTracks[trackIndex];
    float contentStartX = ImGui::GetWindowPos().x + kLabelWidth;

    for (int ci = 0; ci < static_cast<int>(vt.clips.size()); ++ci) {
        const auto& c = vt.clips[ci];
        int clipEnd = c.trackTick + (c.outTick - c.inTick);
        float x1 = contentStartX + static_cast<float>(c.trackTick) * mPixelsPerTick;
        float x2 = contentStartX + static_cast<float>(clipEnd) * mPixelsPerTick;

        if (mousePos.x >= x1 && mousePos.x <= x2) {
            ClipHit hit;
            hit.trackIndex = trackIndex;
            hit.clipIndex = ci;
            hit.onLeftEdge  = (std::abs(mousePos.x - x1) < 6.0f);
            hit.onRightEdge = (std::abs(mousePos.x - x2) < 6.0f);
            return hit;
        }
    }
    return std::nullopt;
}

std::optional<int> TimelinePanel::hitTestKeyframe(
    ImVec2 mousePos, Rect rowArea, int trackIndex)
{
    (void)rowArea;
    auto& state = Editor::getInstance().state();
    float contentStartX = ImGui::GetWindowPos().x + kLabelWidth;

    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.cameraTracks.size()))
        return std::nullopt;

    const auto& track = state.cameraTracks[trackIndex];
    for (int i = 0; i < static_cast<int>(track.keyframes.size()); ++i) {
        float x = contentStartX + static_cast<float>(track.keyframes[i].tick) * mPixelsPerTick;
        float y = (rowArea.min.y + rowArea.max.y) * 0.5f;
        float dist = std::sqrt((mousePos.x - x) * (mousePos.x - x) + (mousePos.y - y) * (mousePos.y - y));
        if (dist < 6.0f) return i;
    }
    return std::nullopt;
}

// ──────────────────────────────────────────────────────────────
//  Zoom
// ──────────────────────────────────────────────────────────────

void TimelinePanel::onWheel(float deltaY) {
    if (ImGui::IsKeyDown(ImGuiKey_LeftShift)) {
        mPixelsPerTick *= (deltaY > 0 ? 1.1f : 1.0f / 1.1f);
        mPixelsPerTick = std::clamp(mPixelsPerTick, 0.02f, 2.0f);
    } else {
        mScrollX -= deltaY * 20.0f;
        mScrollX = std::max(0.0f, mScrollX);
    }
}

// ──────────────────────────────────────────────────────────────
//  Stubs
// ──────────────────────────────────────────────────────────────

void TimelinePanel::drawSelectionRect(Rect fullArea) {
    (void)fullArea;
}

} // namespace playback::refactor::editor