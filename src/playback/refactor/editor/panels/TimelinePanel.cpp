#include "TimelinePanel.h"

#include "playback/refactor/editor/Editor.h"
#include "playback/refactor/editor/EditorBridge.h"
#include "playback/refactor/editor/iconfont.h"

#include "imgui.h"
#include "ll/api/i18n/I18n.h"

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
constexpr float kBasePixelsPerTick = 0.25f;
constexpr float kMinPixelsPerTick = kBasePixelsPerTick * 0.2f;
constexpr float kMaxPixelsPerTick = kBasePixelsPerTick * 20.0f;

bool containsInsensitive(const std::string& value, const std::string& search) {
    if (search.empty()) return true;
    auto it = std::search(value.begin(), value.end(), search.begin(), search.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != value.end();
}

int selectMajorTickStep(float pixelsPerTick) {
    constexpr int steps[] = {20, 40, 100, 200, 400, 600, 1200, 2400, 6000, 12000, 24000, 60000, 120000};
    for (int step : steps) if (step * pixelsPerTick >= 60.0f) return step;
    return steps[std::size(steps) - 1];
}

void formatTimelineTick(char* buffer, size_t size, int tick) {
    int totalSeconds = std::max(0, tick) / 20;
    int hours = totalSeconds / 3600;
    int minutes = (totalSeconds / 60) % 60;
    int seconds = totalSeconds % 60;
    if (hours > 0) std::snprintf(buffer, size, "%02d:%02d:%02d", hours, minutes, seconds);
    else std::snprintf(buffer, size, "%02d:%02d", minutes, seconds);
}
}

void TimelinePanel::setViewPreferences(float trackListWidthRatio, float pixelsPerTick, float horizontalScroll) {
    mTrackListWidthRatio = std::clamp(trackListWidthRatio, 0.18f, 0.55f);
    mPixelsPerTick = std::clamp(pixelsPerTick, kMinPixelsPerTick, kMaxPixelsPerTick);
    mScrollX = std::max(0.0f, horizontalScroll);
}

void TimelinePanel::draw() {
    auto& state = Editor::getInstance().state();
    if (mPendingSeekTick >= 0 && state.currentTick == mPendingSeekTick) mPendingSeekTick = -1;
    if (!mRulerScrubbing && mPendingSeekTick < 0) mPlayheadTick = state.currentTick;
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
    using ll::i18n_literals::operator""_tr;
    auto& state = Editor::getInstance().state();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {7, 7});
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
    if (ImGui::Button(ICON_ADD_MARKER, {28, 28})) EditorBridge::getInstance().addMarker(state, "playback.refactorEditor.defaults.marker"_tr(), state.currentTick);
    ImGui::SameLine();
    ImGui::TextUnformatted("playback.refactorEditor.timeline.snap"_tr().c_str()); ImGui::SameLine(); ImGui::Checkbox("##snap", &mSnapEnabled); ImGui::SameLine();
    ImGui::TextUnformatted("playback.refactorEditor.timeline.scale"_tr().c_str()); ImGui::SameLine();
    if (ImGui::Button("-", {28, 28})) adjustTimeScale(0.9f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(76.0f);
    float scalePercent = mPixelsPerTick / kBasePixelsPerTick * 100.0f;
    if (ImGui::DragFloat("##time-scale", &scalePercent, 1.0f, 20.0f, 2000.0f, "%.0f%%")) {
        float oldPixelsPerTick = mPixelsPerTick;
        mPixelsPerTick = std::clamp(scalePercent / 100.0f * kBasePixelsPerTick, kMinPixelsPerTick, kMaxPixelsPerTick);
        mScrollX = std::max(0.0f, mScrollX + mPlayheadTick * (mPixelsPerTick - oldPixelsPerTick));
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "playback.refactorEditor.timeline.scaleHint"_tr().c_str());
    ImGui::SameLine();
    if (ImGui::Button("+", {28, 28})) adjustTimeScale(1.1f);
    ImGui::PopStyleVar();
}

void TimelinePanel::drawTrackList() {
    using ll::i18n_literals::operator""_tr;
    auto& state = Editor::getInstance().state();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 6.0f);
    char searchBuffer[128]{};
    std::snprintf(searchBuffer, sizeof(searchBuffer), "%s", mTrackSearch.c_str());
    if (ImGui::InputTextWithHint("##track-search", "playback.refactorEditor.timeline.search"_tr().c_str(), searchBuffer, sizeof(searchBuffer))) {
        mTrackSearch = searchBuffer;
    }
    ImGui::BeginDisabled(); ImGui::Button("playback.refactorEditor.timeline.addTrack"_tr().c_str(), {80, 28}); ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", "playback.refactorEditor.timeline.backendUnavailable"_tr().c_str());
    auto group = [](const char* label, bool& open) { ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5); return ImGui::CollapsingHeader(label, &open, ImGuiTreeNodeFlags_DefaultOpen); };
    if (group("playback.refactorEditor.timeline.video"_tr().c_str(), mVideoGroupOpen)) for (const auto& track : state.videoTracks) if (track.visible && containsInsensitive(track.name, mTrackSearch)) ImGui::Text("V  %s%s", track.name.c_str(), track.locked ? "  LOCK" : "");
    if (group("playback.refactorEditor.timeline.camera"_tr().c_str(), mCameraGroupOpen)) for (const auto& track : state.cameraTracks) if (track.visible && containsInsensitive(track.name, mTrackSearch)) ImGui::Text("C  %s%s", track.name.c_str(), track.muted ? "  MUTE" : "");
    if (group("playback.refactorEditor.timeline.markers"_tr().c_str(), mMarkerGroupOpen) && containsInsensitive("Markers", mTrackSearch)) ImGui::Text("M  %s", "playback.refactorEditor.timeline.markers"_tr().c_str());
}

void TimelinePanel::drawRuler() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Rect area{{ImGui::GetCursorScreenPos()}, {ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x, ImGui::GetCursorScreenPos().y + kRulerHeight}};
    dl->AddRectFilled(area.min, area.max, IM_COL32(24, 25, 30, 255));
    int majorStep = selectMajorTickStep(mPixelsPerTick);
    int minorStep = std::max(1, majorStep / 5);
    int firstTick = std::max(0, static_cast<int>(std::floor(mScrollX / mPixelsPerTick / minorStep)) * minorStep);
    float lastLabelRight = area.min.x - 60.0f;
    for (int tick = firstTick; tick <= Editor::getInstance().state().totalTicks; tick += minorStep) {
        float x = area.min.x + tick * mPixelsPerTick - mScrollX;
        if (x < area.min.x || x > area.max.x) continue;
        bool major = tick % majorStep == 0;
        dl->AddLine({x, area.max.y - (major ? 12.0f : 6.0f)}, {x, area.max.y}, major ? IM_COL32(150, 153, 162, 255) : IM_COL32(74, 77, 86, 255));
        if (!major) continue;
        char label[16];
        formatTimelineTick(label, sizeof(label), tick);
        float labelWidth = ImGui::CalcTextSize(label).x;
        if (x >= lastLabelRight + 60.0f && x + labelWidth <= area.max.x) {
            dl->AddText(ImGui::GetFont(), kTextSize, {x + 3, area.min.y + 3}, IM_COL32(180, 182, 190, 255), label);
            lastLabelRight = x + labelWidth;
        }
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
void TimelinePanel::handleRulerClick(Rect area) {
    bool hovered = area.contains(ImGui::GetMousePos());
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) mRulerScrubbing = true;
    if (!mRulerScrubbing) return;
    int tick = std::clamp(static_cast<int>((ImGui::GetMousePos().x - area.min.x + mScrollX) / mPixelsPerTick), 0, Editor::getInstance().state().totalTicks);
    mPlayheadTick = tick;
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        EditorBridge::getInstance().seek(tick);
        mPendingSeekTick = tick;
        mRulerScrubbing = false;
    }
}
void TimelinePanel::drawTransportControls() {
    using ll::i18n_literals::operator""_tr;
    auto& bridge = EditorBridge::getInstance();
    auto& state = Editor::getInstance().state();
    auto transportButton = [](const char* id, auto drawIcon) {
        constexpr float buttonSize = 32.0f;
        ImGui::InvisibleButton(id, {buttonSize, buttonSize});
        ImVec2 min = ImGui::GetItemRectMin();
        ImVec2 max = ImGui::GetItemRectMax();
        ImU32 color = ImGui::IsItemHovered() ? IM_COL32(240, 192, 32, 255) : IM_COL32(220, 222, 230, 255);
        drawIcon(ImGui::GetWindowDrawList(), {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f}, color);
        return ImGui::IsItemClicked();
    };
    if (transportButton("##skip-start", [](ImDrawList* dl, ImVec2 c, ImU32 color) { dl->AddLine({c.x - 9, c.y - 8}, {c.x - 9, c.y + 8}, color, 2); dl->AddTriangleFilled({c.x - 7, c.y}, {c.x + 7, c.y - 8}, {c.x + 7, c.y + 8}, color); })) { bridge.skipToStart(); }
    ImGui::SameLine();
    if (transportButton("##seek-back", [](ImDrawList* dl, ImVec2 c, ImU32 color) { dl->AddTriangleFilled({c.x - 9, c.y}, {c.x + 5, c.y - 8}, {c.x + 5, c.y + 8}, color); dl->AddTriangleFilled({c.x - 2, c.y}, {c.x + 10, c.y - 8}, {c.x + 10, c.y + 8}, color); })) { bridge.seek(std::max(0, state.currentTick - 200)); }
    ImGui::SameLine();
    if (transportButton("##play-pause", [&state](ImDrawList* dl, ImVec2 c, ImU32 color) { if (state.playing) { dl->AddRectFilled({c.x - 7, c.y - 8}, {c.x - 2, c.y + 8}, color); dl->AddRectFilled({c.x + 2, c.y - 8}, {c.x + 7, c.y + 8}, color); } else dl->AddTriangleFilled({c.x - 6, c.y - 9}, {c.x - 6, c.y + 9}, {c.x + 9, c.y}, color); })) bridge.playPause();
    ImGui::SameLine();
    if (transportButton("##seek-forward", [](ImDrawList* dl, ImVec2 c, ImU32 color) { dl->AddTriangleFilled({c.x - 10, c.y - 8}, {c.x - 10, c.y + 8}, {c.x + 2, c.y}, color); dl->AddTriangleFilled({c.x - 3, c.y - 8}, {c.x - 3, c.y + 8}, {c.x + 9, c.y}, color); })) { bridge.seek(std::min(state.totalTicks, state.currentTick + 200)); }
    ImGui::SameLine();
    if (transportButton("##skip-end", [](ImDrawList* dl, ImVec2 c, ImU32 color) { dl->AddTriangleFilled({c.x - 7, c.y - 8}, {c.x - 7, c.y + 8}, {c.x + 7, c.y}, color); dl->AddLine({c.x + 9, c.y - 8}, {c.x + 9, c.y + 8}, color, 2); })) bridge.skipToEnd();
    ImGui::BeginDisabled(); ImGui::Button("playback.refactorEditor.timeline.loop"_tr().c_str(), {52, 28}); ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", "playback.refactorEditor.timeline.backendUnavailable"_tr().c_str());
}
void TimelinePanel::handleSplitAtPlayhead() { if (mSelectedTrackIndex >= 0 && !mSelectedClipId.empty()) { auto& state = Editor::getInstance().state(); if (mSelectedTrackIndex < static_cast<int>(state.videoTracks.size())) EditorBridge::getInstance().splitClip(state, state.videoTracks[mSelectedTrackIndex].id, mSelectedClipId, mPlayheadTick); } }
void TimelinePanel::handleRippleDelete() { if (mSelectedTrackIndex >= 0 && !mSelectedClipId.empty()) { auto& state = Editor::getInstance().state(); if (mSelectedTrackIndex < static_cast<int>(state.videoTracks.size())) EditorBridge::getInstance().deleteClip(state, state.videoTracks[mSelectedTrackIndex].id, mSelectedClipId); mSelectedClipId.clear(); } }
void TimelinePanel::handleTrimLeftToPlayhead() {}
void TimelinePanel::handleTrimRightToPlayhead() {}
void TimelinePanel::commitDragOperation() { auto& state = Editor::getInstance().state(); if (mDragTargetId.empty()) return; if (mDragType == DragType::MoveKeyframe) return; if (mDragTrackIndex < 0 || mDragTrackIndex >= static_cast<int>(state.videoTracks.size())) return; auto& track = state.videoTracks[mDragTrackIndex]; for (auto& clip : track.clips) if (clip.id == mDragTargetId) { if (mDragType == DragType::MoveClip) { int updated = clip.trackTick; clip.trackTick = mDragStartTick; EditorBridge::getInstance().moveClip(state, track.id, clip.id, updated); } else if (mDragType == DragType::TrimClipIn || mDragType == DragType::TrimClipOut) { int in = clip.inTick, out = clip.outTick; if (mDragType == DragType::TrimClipIn) { clip.inTick = mDragOrigTick; clip.trackTick = mDragStartTick; } else clip.outTick = mDragOrigTick; EditorBridge::getInstance().trimClip(state, track.id, clip.id, in, out); } break; } }
void TimelinePanel::onWheel(float deltaY) { if (ImGui::GetIO().KeyShift) adjustTimeScale(deltaY > 0 ? 1.1f : 1.0f / 1.1f); else mScrollX = std::max(0.0f, mScrollX - deltaY * 40.0f); }

void TimelinePanel::adjustTimeScale(float multiplier) {
    float oldPixelsPerTick = mPixelsPerTick;
    mPixelsPerTick = std::clamp(oldPixelsPerTick * multiplier, kMinPixelsPerTick, kMaxPixelsPerTick);
    mScrollX = std::max(0.0f, mScrollX + mPlayheadTick * (mPixelsPerTick - oldPixelsPerTick));
}

} // namespace playback::refactor::editor
