#include "TimelinePanel.h"

#include "playback/refactor/editor/Editor.h"
#include "playback/refactor/editor/EditorBridge.h"
#include "playback/refactor/editor/iconfont.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace playback::refactor::editor {

namespace {
constexpr float kToolbarHeight = 38.0f;
constexpr float kTransportHeight = 34.0f;
constexpr float kSplitterThickness = 4.0f;
constexpr float kRowGap = 2.0f;
constexpr float kTextSize = 14.0f;

bool containsInsensitive(const std::string& value, const std::string& search) {
    if (search.empty()) return true;
    auto it = std::search(value.begin(), value.end(), search.begin(), search.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != value.end();
}
}

void TimelinePanel::setViewPreferences(float trackListWidthRatio, float pixelsPerTick, float horizontalScroll) {
    mTrackListWidthRatio = std::clamp(trackListWidthRatio, 0.18f, 0.55f);
    mPixelsPerTick = std::clamp(pixelsPerTick, 0.02f, 2.0f);
    mScrollX = std::max(0.0f, horizontalScroll);
}

void TimelinePanel::draw() {
    auto& state = Editor::getInstance().state();
    mPlayheadTick = state.currentTick;
    Rect full{{ImGui::GetCursorScreenPos()}, {ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x,
                                               ImGui::GetCursorScreenPos().y + ImGui::GetContentRegionAvail().y}};
    if (full.GetWidth() < 80.0f || full.GetHeight() < 100.0f) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(full.min, full.max, IM_COL32(18, 19, 23, 255));

    ImGui::SetCursorScreenPos(full.min);
    drawHeader();
    float workTop = full.min.y + kToolbarHeight;
    float workBottom = full.max.y - kTransportHeight;
    float workWidth = full.GetWidth();
    float listWidth = std::clamp(workWidth * mTrackListWidthRatio, 160.0f, std::max(160.0f, workWidth - 180.0f));
    Rect listArea{{full.min.x, workTop}, {full.min.x + listWidth, workBottom}};
    Rect canvasArea{{listArea.max.x + kSplitterThickness, workTop}, {full.max.x, workBottom}};

    ImGui::SetCursorScreenPos({listArea.max.x - kSplitterThickness * 0.5f, workTop});
    ImGui::InvisibleButton("##timeline-track-list-splitter", {kSplitterThickness, listArea.GetHeight()});
    bool splitterActive = ImGui::IsItemActive();
    if (ImGui::IsItemHovered() || splitterActive) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (splitterActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        mTrackListWidthRatio = std::clamp((ImGui::GetMousePos().x - full.min.x) / workWidth, 0.18f, 0.55f);
        listWidth = workWidth * mTrackListWidthRatio;
        listArea.max.x = full.min.x + listWidth;
        canvasArea.min.x = listArea.max.x + kSplitterThickness;
    }
    dl->AddLine({listArea.max.x, workTop}, {listArea.max.x, workBottom}, splitterActive ? IM_COL32(240, 192, 32, 255) : IM_COL32(75, 77, 86, 255), 2.0f);

    ImGui::SetCursorScreenPos(listArea.min);
    ImGui::BeginChild("##TimelineTrackList", {listArea.GetWidth(), listArea.GetHeight()}, false, ImGuiWindowFlags_NoScrollbar);
    drawTrackList();
    ImGui::EndChild();

    ImGui::SetCursorScreenPos(canvasArea.min);
    ImGui::BeginChild("##TimelineCanvas", {canvasArea.GetWidth(), canvasArea.GetHeight()}, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawRuler();
    drawBody();
    ImGui::EndChild();

    ImGui::SetCursorScreenPos({full.min.x, workBottom});
    drawTransportControls();
    if (mDragType != DragType::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        commitDragOperation();
        mDragType = DragType::None;
        mDragTargetId.clear();
    }
}

void TimelinePanel::drawHeader() {
    auto& state = Editor::getInstance().state();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {7, 7});
    if (ImGui::Button(state.playing ? ICON_PAUSE : ICON_PLAY, {28, 28})) EditorBridge::getInstance().playPause();
    ImGui::SameLine();
    int seconds = state.currentTick / 20;
    char timecode[48];
    std::snprintf(timecode, sizeof(timecode), "%02d:%02d:%02d.%03d / %02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60, (state.currentTick % 20) * 50, state.totalTicks / 1200, (state.totalTicks / 20) % 60);
    ImGui::TextUnformatted(timecode);
    ImGui::SameLine();
    if (ImGui::Button(ICON_UNDO, {28, 28})) EditorBridge::getInstance().undo(state);
    ImGui::SameLine();
    if (ImGui::Button(ICON_REDO, {28, 28})) EditorBridge::getInstance().redo(state);
    ImGui::SameLine();
    if (ImGui::Button(ICON_ADD_KEYFRAME, {28, 28})) {
        for (auto& track : state.cameraTracks) if (track.active) { EditorBridge::getInstance().addKeyframe(state, track.id, state.currentTick); break; }
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_ADD_MARKER, {28, 28})) EditorBridge::getInstance().addMarker(state, "Marker", state.currentTick);
    ImGui::SameLine();
    ImGui::TextUnformatted("Snap"); ImGui::SameLine(); ImGui::Checkbox("##snap", &mSnapEnabled); ImGui::SameLine();
    ImGui::Text("Time Scale"); ImGui::SameLine();
    if (ImGui::Button("-", {28, 28})) adjustTimeScale(1.0f / 1.25f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(72.0f);
    float scalePercent = mPixelsPerTick / 0.1f * 100.0f;
    if (ImGui::DragFloat("##time-scale", &scalePercent, 1.0f, 20.0f, 2000.0f, "%.0f%%")) {
        float oldPixelsPerTick = mPixelsPerTick;
        mPixelsPerTick = std::clamp(scalePercent * 0.001f, 0.02f, 2.0f);
        mScrollX = std::max(0.0f, mScrollX + mPlayheadTick * (mPixelsPerTick - oldPixelsPerTick));
    }
    ImGui::SameLine();
    if (ImGui::Button("+", {28, 28})) adjustTimeScale(1.25f);
    ImGui::PopStyleVar();
}

void TimelinePanel::drawTrackList() {
    auto& state = Editor::getInstance().state();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 6.0f);
    char searchBuffer[128]{};
    std::snprintf(searchBuffer, sizeof(searchBuffer), "%s", mTrackSearch.c_str());
    if (ImGui::InputTextWithHint("##track-search", "Search tracks", searchBuffer, sizeof(searchBuffer))) {
        mTrackSearch = searchBuffer;
    }
    ImGui::BeginDisabled(); ImGui::Button("+ Track", {80, 28}); ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("等待后端接口");
    auto group = [](const char* label, bool& open) { ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5); return ImGui::CollapsingHeader(label, &open, ImGuiTreeNodeFlags_DefaultOpen); };
    if (group("Video", mVideoGroupOpen)) for (const auto& track : state.videoTracks) if (track.visible && containsInsensitive(track.name, mTrackSearch)) ImGui::Text("V  %s%s", track.name.c_str(), track.locked ? "  LOCK" : "");
    if (group("Camera", mCameraGroupOpen)) for (const auto& track : state.cameraTracks) if (track.visible && containsInsensitive(track.name, mTrackSearch)) ImGui::Text("C  %s%s", track.name.c_str(), track.muted ? "  MUTE" : "");
    if (group("Markers", mMarkerGroupOpen) && containsInsensitive("Markers", mTrackSearch)) ImGui::TextUnformatted("M  Markers");
}

void TimelinePanel::drawRuler() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Rect area{{ImGui::GetCursorScreenPos()}, {ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x, ImGui::GetCursorScreenPos().y + kRulerHeight}};
    dl->AddRectFilled(area.min, area.max, IM_COL32(24, 25, 30, 255));
    int step = mPixelsPerTick >= 0.5f ? 20 : mPixelsPerTick >= 0.1f ? 100 : 200;
    for (int tick = 0; tick <= Editor::getInstance().state().totalTicks; tick += step) {
        float x = area.min.x + tick * mPixelsPerTick - mScrollX;
        if (x < area.min.x || x > area.max.x) continue;
        dl->AddLine({x, area.max.y - 9}, {x, area.max.y}, IM_COL32(100, 103, 112, 255));
        char label[16]; std::snprintf(label, sizeof(label), "%d:%02d", tick / 1200, (tick / 20) % 60);
        dl->AddText(ImGui::GetFont(), kTextSize, {x + 3, area.min.y + 3}, IM_COL32(180, 182, 190, 255), label);
    }
    handleRulerClick(area);
    ImGui::SetCursorScreenPos({area.min.x, area.max.y});
}

void TimelinePanel::drawBody() {
    auto& state = Editor::getInstance().state();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Rect canvas{{ImGui::GetCursorScreenPos()}, {ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x, ImGui::GetCursorScreenPos().y + ImGui::GetContentRegionAvail().y}};
    dl->PushClipRect(canvas.min, canvas.max, true);
    float y = canvas.min.y;
    auto drawRow = [&](float height) { Rect row{{canvas.min.x, y}, {canvas.max.x, y + height}}; dl->AddRectFilled(row.min, row.max, IM_COL32(28, 29, 34, 255)); dl->AddLine({row.min.x, row.max.y}, row.max, IM_COL32(62, 64, 72, 255)); y += height + kRowGap; return row; };
    if (mVideoGroupOpen) for (int ti = 0; ti < static_cast<int>(state.videoTracks.size()); ++ti) {
        auto& track = state.videoTracks[ti]; if (!track.visible || !containsInsensitive(track.name, mTrackSearch)) continue;
        Rect row = drawRow(std::max(48.0f, static_cast<float>(track.height)));
        for (auto& clip : track.clips) {
            int end = clip.trackTick + clip.outTick - clip.inTick;
            float x1 = row.min.x + clip.trackTick * mPixelsPerTick - mScrollX, x2 = row.min.x + end * mPixelsPerTick - mScrollX;
            Rect clipRect{{x1, row.min.y + 4}, {x2, row.max.y - 4}}; if (clipRect.max.x < row.min.x || clipRect.min.x > row.max.x) continue;
            dl->AddRectFilled(clipRect.min, clipRect.max, IM_COL32(42, 90, 138, 220), 3); dl->AddRect(clipRect.min, clipRect.max, clip.id == mSelectedClipId ? IM_COL32(240, 192, 32, 255) : IM_COL32(91, 149, 201, 255), 3);
            dl->AddText(ImGui::GetFont(), kTextSize, {x1 + 5, row.min.y + 7}, IM_COL32(235, 235, 238, 255), clip.name.c_str());
            bool hovered = clipRect.contains(ImGui::GetMousePos());
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { mSelectedClipId = clip.id; mSelectedTrackIndex = ti; mDragType = (std::abs(ImGui::GetMousePos().x - x1) < 7 ? DragType::TrimClipIn : std::abs(ImGui::GetMousePos().x - x2) < 7 ? DragType::TrimClipOut : DragType::MoveClip); mDragTargetId = clip.id; mDragTrackIndex = ti; mDragStartTick = mDragType == DragType::TrimClipOut ? end : clip.trackTick; mDragOrigTick = mDragType == DragType::TrimClipIn ? clip.inTick : clip.outTick; }
            if (mDragTargetId == clip.id && ImGui::IsMouseDown(ImGuiMouseButton_Left)) { int tick = std::max(0, static_cast<int>((ImGui::GetMousePos().x - row.min.x + mScrollX) / mPixelsPerTick)); if (mDragType == DragType::MoveClip) clip.trackTick = std::clamp(tick, 0, std::max(0, state.totalTicks - (clip.outTick - clip.inTick))); else if (mDragType == DragType::TrimClipIn) { clip.inTick = std::clamp(mDragOrigTick + tick - mDragStartTick, 0, clip.outTick - 1); clip.trackTick = mDragStartTick + clip.inTick - mDragOrigTick; } else if (mDragType == DragType::TrimClipOut) clip.outTick = std::max(clip.inTick + 1, mDragOrigTick + tick - mDragStartTick); }
        }
    }
    if (mCameraGroupOpen) for (auto& track : state.cameraTracks) { if (!track.visible || !containsInsensitive(track.name, mTrackSearch)) continue; Rect row = drawRow(30); for (auto& keyframe : track.keyframes) { float x = row.min.x + keyframe.tick * mPixelsPerTick - mScrollX; dl->AddCircleFilled({x, (row.min.y + row.max.y) * .5f}, 5, IM_COL32(128, 192, 240, 255)); } }
    if (mMarkerGroupOpen && containsInsensitive("Markers", mTrackSearch)) { Rect row = drawRow(28); for (const auto& marker : state.markers) { float x = row.min.x + marker.tick * mPixelsPerTick - mScrollX; dl->AddLine({x, row.min.y}, {x, row.max.y}, IM_COL32(240, 192, 32, 255)); dl->AddText(ImGui::GetFont(), kTextSize, {x + 4, row.min.y + 5}, IM_COL32(240, 210, 100, 255), marker.label.c_str()); } }
    drawPlayhead({{canvas.min.x, canvas.min.y - kRulerHeight}, canvas.max});
    dl->PopClipRect();
    ImGui::SetCursorScreenPos({canvas.min.x, canvas.max.y - 16});
    float maxScroll = std::max(0.0f, state.totalTicks * mPixelsPerTick - canvas.GetWidth());
    ImGui::SetNextItemWidth(canvas.GetWidth()); ImGui::SliderFloat("##timeline-scroll", &mScrollX, 0.0f, maxScroll, "", ImGuiSliderFlags_NoInput);
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) onWheel(ImGui::GetIO().MouseWheel);
}

void TimelinePanel::drawPlayhead(Rect area) { float x = area.min.x + mPlayheadTick * mPixelsPerTick - mScrollX; if (x >= area.min.x && x <= area.max.x) ImGui::GetWindowDrawList()->AddLine({x, area.min.y}, {x, area.max.y}, IM_COL32(240, 192, 32, 255), 2); }
void TimelinePanel::handleRulerClick(Rect area) { if (area.contains(ImGui::GetMousePos()) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) { int tick = std::clamp(static_cast<int>((ImGui::GetMousePos().x - area.min.x + mScrollX) / mPixelsPerTick), 0, Editor::getInstance().state().totalTicks); if (tick != mPlayheadTick) { mPlayheadTick = tick; EditorBridge::getInstance().seek(tick); } } }
void TimelinePanel::drawTransportControls() { auto& bridge = EditorBridge::getInstance(); auto& state = Editor::getInstance().state(); if (ImGui::Button("|<", {28, 28})) bridge.skipToStart(); ImGui::SameLine(); if (ImGui::Button("<", {28, 28})) bridge.seek(std::max(0, state.currentTick - 1)); ImGui::SameLine(); if (ImGui::Button(state.playing ? ICON_PAUSE : ICON_PLAY, {28, 28})) bridge.playPause(); ImGui::SameLine(); if (ImGui::Button(">", {28, 28})) bridge.seek(std::min(state.totalTicks, state.currentTick + 1)); ImGui::SameLine(); if (ImGui::Button(">|", {28, 28})) bridge.skipToEnd(); ImGui::SameLine(); if (ImGui::Button("Speed -", {62, 28})) bridge.decreaseSpeed(); ImGui::SameLine(); if (ImGui::Button("Speed +", {62, 28})) bridge.increaseSpeed(); ImGui::SameLine(); ImGui::BeginDisabled(); ImGui::Button("Loop", {52, 28}); ImGui::EndDisabled(); if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("等待后端接口"); }
void TimelinePanel::handleSplitAtPlayhead() { if (mSelectedTrackIndex >= 0 && !mSelectedClipId.empty()) { auto& state = Editor::getInstance().state(); if (mSelectedTrackIndex < static_cast<int>(state.videoTracks.size())) EditorBridge::getInstance().splitClip(state, state.videoTracks[mSelectedTrackIndex].id, mSelectedClipId, mPlayheadTick); } }
void TimelinePanel::handleRippleDelete() { if (mSelectedTrackIndex >= 0 && !mSelectedClipId.empty()) { auto& state = Editor::getInstance().state(); if (mSelectedTrackIndex < static_cast<int>(state.videoTracks.size())) EditorBridge::getInstance().deleteClip(state, state.videoTracks[mSelectedTrackIndex].id, mSelectedClipId); mSelectedClipId.clear(); } }
void TimelinePanel::handleTrimLeftToPlayhead() {}
void TimelinePanel::handleTrimRightToPlayhead() {}
void TimelinePanel::commitDragOperation() { auto& state = Editor::getInstance().state(); if (mDragTargetId.empty()) return; if (mDragType == DragType::MoveKeyframe) return; if (mDragTrackIndex < 0 || mDragTrackIndex >= static_cast<int>(state.videoTracks.size())) return; auto& track = state.videoTracks[mDragTrackIndex]; for (auto& clip : track.clips) if (clip.id == mDragTargetId) { if (mDragType == DragType::MoveClip) { int updated = clip.trackTick; clip.trackTick = mDragStartTick; EditorBridge::getInstance().moveClip(state, track.id, clip.id, updated); } else if (mDragType == DragType::TrimClipIn || mDragType == DragType::TrimClipOut) { int in = clip.inTick, out = clip.outTick; if (mDragType == DragType::TrimClipIn) { clip.inTick = mDragOrigTick; clip.trackTick = mDragStartTick; } else clip.outTick = mDragOrigTick; EditorBridge::getInstance().trimClip(state, track.id, clip.id, in, out); } break; } }
void TimelinePanel::onWheel(float deltaY) { if (ImGui::GetIO().KeyShift) adjustTimeScale(deltaY > 0 ? 1.1f : 1.0f / 1.1f); else mScrollX = std::max(0.0f, mScrollX - deltaY * 40.0f); }

void TimelinePanel::adjustTimeScale(float multiplier) {
    float oldPixelsPerTick = mPixelsPerTick;
    mPixelsPerTick = std::clamp(oldPixelsPerTick * multiplier, 0.02f, 2.0f);
    mScrollX = std::max(0.0f, mScrollX + mPlayheadTick * (mPixelsPerTick - oldPixelsPerTick));
}

} // namespace playback::refactor::editor
