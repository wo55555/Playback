#include "TimelinePanel.h"

#include "playback/refactor/editor/Editor.h"
#include "playback/refactor/editor/EditorBridge.h"
#include "playback/refactor/editor/iconfont.h"

#include "imgui.h"

#include <algorithm>
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

int selectMajorTickStep(float pixelsPerTick) {
    constexpr int steps[] = {20, 40, 100, 200, 400, 600, 1200, 2400, 6000, 12000, 24000, 60000, 120000};
    for (int step : steps) if (step * pixelsPerTick >= 60.0f) return step;
    return steps[std::size(steps) - 1];
}

void formatTimelineTick(char* buffer, size_t size, int tick) {
    int totalSeconds = std::max(0, tick) / 20;
    std::snprintf(buffer, size, "%02d:%02d", (totalSeconds / 60) % 60, totalSeconds % 60);
}

ImU32 toColor(const Color4& color, int alpha = 220) {
    return IM_COL32(
        static_cast<int>(color.r * 255.0f), static_cast<int>(color.g * 255.0f),
        static_cast<int>(color.b * 255.0f), alpha);
}

template <typename T>
bool isSelected(const SelectionModel& selection) {
    return selection.getAs<T>() != nullptr;
}

}

void TimelinePanel::setViewPreferences(float trackListWidthRatio, float pixelsPerTick, float horizontalScroll) {
    mTrackListWidthRatio = std::clamp(trackListWidthRatio, 0.18f, 0.55f);
    mPixelsPerTick = std::clamp(pixelsPerTick, kMinPixelsPerTick, kMaxPixelsPerTick);
    mScrollX = std::max(0.0f, horizontalScroll);
}

void TimelinePanel::draw() {
    auto& state = Editor::getInstance().state();
    mTrackTree.setSearch(mTrackSearch);
    mTrackTree.rebuild(state);
    if (mPendingSeekTick >= 0 && state.currentTick == mPendingSeekTick) mPendingSeekTick = -1;
    if (!mRulerScrubbing && mPendingSeekTick < 0) mPlayheadTick = state.currentTick;
    Rect full{{ImGui::GetCursorScreenPos()}, {ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x,
                                               ImGui::GetCursorScreenPos().y + ImGui::GetContentRegionAvail().y}};
    if (full.GetWidth() < 80.0f || full.GetHeight() < 100.0f) return;
    auto* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(full.min, full.max, IM_COL32(18, 19, 23, 255));
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
        listArea.max.x = full.min.x + workWidth * mTrackListWidthRatio;
        canvasArea.min.x = listArea.max.x + kSplitterThickness;
    }
    drawList->AddLine({listArea.max.x, workTop}, {listArea.max.x, workBottom}, splitterActive ? IM_COL32(240, 192, 32, 255) : IM_COL32(75, 77, 86, 255), 2.0f);
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
}

void TimelinePanel::drawHeader() {
    auto& state = Editor::getInstance().state();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {7, 7});
    char timecode[48];
    std::snprintf(timecode, sizeof(timecode), "%d / %d ticks", state.currentTick, state.totalTicks);
    ImGui::TextUnformatted(timecode);
    ImGui::SameLine();
    if (ImGui::Button(ICON_UNDO, {28, 28})) EditorBridge::getInstance().undo(state);
    ImGui::SameLine();
    if (ImGui::Button(ICON_REDO, {28, 28})) EditorBridge::getInstance().redo(state);
    ImGui::SameLine();
    const auto* selectedCamera = Editor::getInstance().selection().getAs<SelectedCamera>();
    ImGui::BeginDisabled(selectedCamera == nullptr);
    if (ImGui::Button(ICON_ADD_KEYFRAME, {28, 28}) && selectedCamera) {
        EditorBridge::getInstance().addCameraKeyframe(state, selectedCamera->cameraId, state.currentTick);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ICON_ADD_MARKER, {28, 28})) EditorBridge::getInstance().addMarker(state, "Marker", state.currentTick);
    ImGui::SameLine();
    ImGui::TextUnformatted("Snap"); ImGui::SameLine(); ImGui::Checkbox("##snap", &mSnapEnabled); ImGui::SameLine();
    if (ImGui::Button("-", {28, 28})) adjustTimeScale(0.9f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(76.0f);
    float scalePercent = mPixelsPerTick / kBasePixelsPerTick * 100.0f;
    if (ImGui::DragFloat("##time-scale", &scalePercent, 1.0f, 20.0f, 2000.0f, "%.0f%%")) adjustTimeScale(scalePercent / (mPixelsPerTick / kBasePixelsPerTick * 100.0f));
    ImGui::SameLine();
    if (ImGui::Button("+", {28, 28})) adjustTimeScale(1.1f);
    ImGui::PopStyleVar();
}

void TimelinePanel::drawTrackList() {
    char searchBuffer[128]{};
    std::snprintf(searchBuffer, sizeof(searchBuffer), "%s", mTrackSearch.c_str());
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 6.0f);
    if (ImGui::InputTextWithHint("##track-search", "Search cameras", searchBuffer, sizeof(searchBuffer))) mTrackSearch = searchBuffer;
    const auto& selection = Editor::getInstance().selection();
    for (const auto& row : mTrackTree.rows()) {
        bool selected = (row.kind == TrackRowKind::Sequence && isSelected<SelectedSequence>(selection))
            || (row.kind == TrackRowKind::WorldActor && isSelected<SelectedWorldActor>(selection))
            || (row.kind == TrackRowKind::Camera && selection.getAs<SelectedCamera>() && selection.getAs<SelectedCamera>()->cameraId == row.id.substr(7));
        if (selected) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(240, 192, 32, 255));
        const char* prefix = row.kind == TrackRowKind::Sequence ? "S" : row.kind == TrackRowKind::WorldActor ? "W" : row.kind == TrackRowKind::Camera ? "C" : "M";
        ImGui::Text("%s  %s%s", prefix, row.name.c_str(), row.locked ? "  LOCK" : "");
        if (selected) ImGui::PopStyleColor();
        if (row.kind == TrackRowKind::Camera && row.active) { ImGui::SameLine(); ImGui::TextDisabled("ACTIVE"); }
    }
}

void TimelinePanel::drawRuler() {
    auto* drawList = ImGui::GetWindowDrawList();
    Rect area{{ImGui::GetCursorScreenPos()}, {ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x, ImGui::GetCursorScreenPos().y + kRulerHeight}};
    drawList->AddRectFilled(area.min, area.max, IM_COL32(24, 25, 30, 255));
    int majorStep = selectMajorTickStep(mPixelsPerTick);
    int minorStep = std::max(1, majorStep / 5);
    int firstTick = std::max(0, static_cast<int>(std::floor(mScrollX / mPixelsPerTick / minorStep)) * minorStep);
    for (int tick = firstTick; tick <= Editor::getInstance().state().totalTicks; tick += minorStep) {
        float x = area.min.x + tick * mPixelsPerTick - mScrollX;
        if (x < area.min.x || x > area.max.x) continue;
        bool major = tick % majorStep == 0;
        drawList->AddLine({x, area.max.y - (major ? 12.0f : 6.0f)}, {x, area.max.y}, major ? IM_COL32(150, 153, 162, 255) : IM_COL32(74, 77, 86, 255));
        if (major) { char label[16]; formatTimelineTick(label, sizeof(label), tick); drawList->AddText({x + 3, area.min.y + 3}, IM_COL32(180, 182, 190, 255), label); }
    }
    handleRulerClick(area);
    ImGui::SetCursorScreenPos({area.min.x, area.max.y});
}

void TimelinePanel::drawBody() {
    auto& editor = Editor::getInstance();
    auto& state = editor.state();
    auto* drawList = ImGui::GetWindowDrawList();
    Rect canvas{{ImGui::GetCursorScreenPos()}, {ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x, ImGui::GetCursorScreenPos().y + ImGui::GetContentRegionAvail().y}};
    drawList->PushClipRect(canvas.min, canvas.max, true);
    float y = canvas.min.y;
    auto selectRow = [&editor](const TrackTreeRow& row) {
        if (row.kind == TrackRowKind::Sequence) editor.selection().select(SelectedSequence{});
        else if (row.kind == TrackRowKind::WorldActor) editor.selection().select(SelectedWorldActor{});
        else if (row.kind == TrackRowKind::Camera) editor.selection().select(SelectedCamera{row.id.substr(7)});
        else editor.selection().clear();
    };
    for (const auto& rowInfo : mTrackTree.rows()) {
        Rect row{{canvas.min.x, y}, {canvas.max.x, y + rowInfo.height}};
        drawList->AddRectFilled(row.min, row.max, IM_COL32(28, 29, 34, 255));
        drawList->AddLine({row.min.x, row.max.y}, row.max, IM_COL32(62, 64, 72, 255));
        if (row.contains(ImGui::GetMousePos()) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) selectRow(rowInfo);
        if (rowInfo.kind == TrackRowKind::Sequence) {
            for (const auto& segment : state.sequence) {
                Rect segmentRect{{row.min.x + segment.startTick * mPixelsPerTick - mScrollX, row.min.y + 4}, {row.min.x + segment.endTick * mPixelsPerTick - mScrollX, row.max.y - 4}};
                auto* selection = editor.selection().getAs<SelectedSequenceSegment>();
                bool selected = selection && selection->segmentId == segment.id;
                drawList->AddRectFilled(segmentRect.min, segmentRect.max, toColor(segment.color), 3.0f);
                drawList->AddRect(segmentRect.min, segmentRect.max, selected ? IM_COL32(240, 192, 32, 255) : IM_COL32(91, 149, 201, 255), 3.0f);
                const char* name = segment.cameraId.empty() ? "Auto (first camera)" : segment.cameraId.c_str();
                drawList->AddText({segmentRect.min.x + 5, segmentRect.min.y + 6}, IM_COL32(235, 235, 238, 255), name);
                if (segmentRect.contains(ImGui::GetMousePos()) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) editor.selection().select(SelectedSequenceSegment{segment.id});
            }
        } else if (rowInfo.kind == TrackRowKind::WorldActor) {
            for (const auto& segment : state.worldActor.segments) {
                Rect segmentRect{{row.min.x + segment.startTick * mPixelsPerTick - mScrollX, row.min.y + 4}, {row.min.x + segment.endTick * mPixelsPerTick - mScrollX, row.max.y - 4}};
                auto* selection = editor.selection().getAs<SelectedWorldActorSegment>();
                bool selected = selection && selection->segmentId == segment.id;
                drawList->AddRectFilled(segmentRect.min, segmentRect.max, toColor(segment.color), 3.0f);
                drawList->AddRect(segmentRect.min, segmentRect.max, selected ? IM_COL32(240, 192, 32, 255) : IM_COL32(214, 132, 62, 255), 3.0f);
                char label[48]; std::snprintf(label, sizeof(label), "%.2fx", segment.speed);
                drawList->AddText({segmentRect.min.x + 5, segmentRect.min.y + 6}, IM_COL32(235, 235, 238, 255), label);
                if (segmentRect.contains(ImGui::GetMousePos()) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) editor.selection().select(SelectedWorldActorSegment{segment.id});
            }
        } else if (rowInfo.kind == TrackRowKind::Camera && rowInfo.cameraIndex >= 0 && rowInfo.cameraIndex < static_cast<int>(state.cameras.size())) {
            const auto& camera = state.cameras[rowInfo.cameraIndex];
            for (const auto& keyframe : camera.keys) {
                float x = row.min.x + keyframe.tick * mPixelsPerTick - mScrollX;
                drawList->AddCircleFilled({x, (row.min.y + row.max.y) * 0.5f}, 5.0f, IM_COL32(128, 192, 240, 255));
                if (std::abs(ImGui::GetMousePos().x - x) <= 7.0f && row.contains(ImGui::GetMousePos()) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) editor.selection().select(SelectedKeyframe{camera.id, keyframe.id});
            }
        } else if (rowInfo.kind == TrackRowKind::Marker) {
            for (const auto& marker : state.markers) {
                float x = row.min.x + marker.tick * mPixelsPerTick - mScrollX;
                drawList->AddLine({x, row.min.y}, {x, row.max.y}, IM_COL32(240, 192, 32, 255));
                drawList->AddText({x + 4, row.min.y + 3}, IM_COL32(240, 210, 100, 255), marker.label.c_str());
            }
        }
        y += rowInfo.height + kRowGap;
    }
    drawPlayhead({{canvas.min.x, canvas.min.y - kRulerHeight}, canvas.max});
    drawList->PopClipRect();
    ImGui::SetCursorScreenPos({canvas.min.x, canvas.max.y - 16});
    float maxScroll = std::max(0.0f, state.totalTicks * mPixelsPerTick - canvas.GetWidth());
    ImGui::SetNextItemWidth(canvas.GetWidth());
    ImGui::SliderFloat("##timeline-scroll", &mScrollX, 0.0f, maxScroll, "", ImGuiSliderFlags_NoInput);
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) onWheel(ImGui::GetIO().MouseWheel);
}

void TimelinePanel::drawPlayhead(Rect area) {
    float x = area.min.x + mPlayheadTick * mPixelsPerTick - mScrollX;
    if (x >= area.min.x && x <= area.max.x) ImGui::GetWindowDrawList()->AddLine({x, area.min.y}, {x, area.max.y}, IM_COL32(240, 192, 32, 255), 2.0f);
}

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
    auto& bridge = EditorBridge::getInstance();
    auto& state = Editor::getInstance().state();
    if (ImGui::Button("|<", {32, 28})) bridge.skipToStart(); ImGui::SameLine();
    if (ImGui::Button("<<", {32, 28})) bridge.seek(std::max(0, state.currentTick - 200)); ImGui::SameLine();
    if (ImGui::Button(state.playing ? "Pause" : "Play", {52, 28})) bridge.playPause(); ImGui::SameLine();
    if (ImGui::Button(">>", {32, 28})) bridge.seek(std::min(state.totalTicks, state.currentTick + 200)); ImGui::SameLine();
    if (ImGui::Button(">|", {32, 28})) bridge.skipToEnd(); ImGui::SameLine();
    ImGui::BeginDisabled(); ImGui::Button("Loop", {52, 28}); ImGui::EndDisabled();
}

void TimelinePanel::onWheel(float deltaY) {
    if (ImGui::GetIO().KeyShift) adjustTimeScale(deltaY > 0 ? 1.1f : 1.0f / 1.1f);
    else mScrollX = std::max(0.0f, mScrollX - deltaY * 40.0f);
}

void TimelinePanel::adjustTimeScale(float multiplier) {
    float oldPixelsPerTick = mPixelsPerTick;
    mPixelsPerTick = std::clamp(oldPixelsPerTick * multiplier, kMinPixelsPerTick, kMaxPixelsPerTick);
    mScrollX = std::max(0.0f, mScrollX + mPlayheadTick * (mPixelsPerTick - oldPixelsPerTick));
}

} // namespace playback::refactor::editor
