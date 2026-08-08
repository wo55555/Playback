#include "TimelinePanel.h"

#include "playback/editor/ui/ReplayEditor.h"
#include "playback/editor/ui/iconfont.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace playback::editor::ui {

using namespace ll::i18n_literals;

namespace {

constexpr float kSplitterThickness = 4.0f;
constexpr float kMinZoomScale      = 1.0f;
constexpr float kMaxZoomScale      = 20.0f;
constexpr float kZoomStep          = 1.15f;

constexpr ImU32 kBackground = IM_COL32(27, 27, 27, 255);
constexpr ImU32 kSidebarBackground = IM_COL32(41, 41, 41, 255);
constexpr ImU32 kRulerBackground = IM_COL32(32, 32, 32, 255);
constexpr ImU32 kLine = IM_COL32(73, 73, 73, 255);
constexpr ImU32 kSequenceColor = IM_COL32(98, 98, 98, 255);
constexpr ImU32 kCameraColor = IM_COL32(77, 63, 83, 255);
constexpr ImU32 kPlayheadColor = IM_COL32(58, 140, 240, 255);

float iconButtonSize() { return std::max(25.0f, ImGui::GetFontSize() + 12.0f); }

bool iconButton(char const* id, char const* icon, char const* tooltip, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    float const buttonSize = iconButtonSize();
    ImVec2 const cursor = ImGui::GetCursorScreenPos();
    ImVec2 const mouse = ImGui::GetMousePos();
    bool const hovered = enabled && mouse.x >= cursor.x && mouse.x <= cursor.x + buttonSize && mouse.y >= cursor.y && mouse.y <= cursor.y + buttonSize;
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(170, 170, 170, 255));
    bool const clicked = ImGui::Button((std::string(icon) + "##" + id).c_str(), {buttonSize, buttonSize});
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && tooltip) ImGui::SetTooltip("%s", tooltip);
    ImGui::EndDisabled();
    return clicked;
}

std::string formatTick(int tick) {
    char value[32]{};
    tick = std::max(0, tick);
    std::snprintf(value, sizeof(value), "%02d:%02d", tick / 1200, (tick / 20) % 60);
    return value;
}

int majorTickStep(float pixelsPerTick, float minimumSpacing) {
    constexpr int steps[] = {20, 40, 100, 200, 400, 600, 1200, 2400, 6000, 12000};
    for (int step : steps)
        if (step * pixelsPerTick >= minimumSpacing) return step;
    return steps[std::size(steps) - 1];
}

ImU32 color(editing::model::Color4 const& value, int alpha = 220) {
    return IM_COL32(
        static_cast<int>(value.r * 255.0f),
        static_cast<int>(value.g * 255.0f),
        static_cast<int>(value.b * 255.0f),
        alpha
    );
}

bool contains(ImVec2 const& minimum, ImVec2 const& maximum, ImVec2 const& point) {
    return point.x >= minimum.x && point.x <= maximum.x && point.y >= minimum.y && point.y <= maximum.y;
}

} // namespace

void TimelinePanel::setViewPreferences(float trackListWidthRatio, float zoomScale, float horizontalScroll) {
    mTrackListWidthRatio = std::clamp(trackListWidthRatio, 0.18f, 0.55f);
    mZoomScale           = std::clamp(zoomScale, kMinZoomScale, kMaxZoomScale);
    mScrollX             = std::max(0.0f, horizontalScroll);
    mScrollY             = 0.0f;
    mPendingSeekTick     = -1;
    mDraggingSegmentId.clear();
}

void TimelinePanel::submitSeek(int tick) {
    auto const& state = ReplayEditor::getInstance().state();
    mPendingSeekTick  = std::clamp(tick, 0, std::max(0, state.totalTicks));
    EditorAction action{EditorActionType::Seek};
    action.tick = mPendingSeekTick;
    submitEdit(std::move(action));
}

void TimelinePanel::submitEdit(EditorAction action) { ReplayEditor::getInstance().submitAction(std::move(action)); }

void TimelinePanel::draw() {
    auto&       editor  = ReplayEditor::getInstance();
    auto const& state   = editor.state();
    auto const  project = state.project;
    if (!project) {
        ImGui::TextDisabled("%s", "playback.refactorEditor.common.noActiveProject"_tr().c_str());
        return;
    }

    mTrackTree.setSearch(mTrackSearch);
    mTrackTree.setCamerasExpanded(mCamerasExpanded);
    mTrackTree.rebuild(*project);
    int displayTick = mPendingSeekTick >= 0 ? mPendingSeekTick : state.currentTick;
    if (mPendingSeekTick >= 0 && state.currentTick == mPendingSeekTick) mPendingSeekTick = -1;

    ImVec2 const fullMin   = ImGui::GetCursorScreenPos();
    ImVec2 const available = ImGui::GetContentRegionAvail();
    if (available.x < 220.0f || available.y < 120.0f) return;
    ImVec2 const fullMax{fullMin.x + available.x, fullMin.y + available.y};
    float const fontSize = ImGui::GetFontSize();
    float const toolbarHeight = fontSize + 16.0f;
    float const transportHeight = iconButtonSize() + 8.0f;
    float const rulerHeight = fontSize + 18.0f;
    auto* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(fullMin, fullMax, kBackground);
    float const zoomScaleBeforeToolbar = mZoomScale;

    ImGui::SetCursorScreenPos(fullMin);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild("##TimelineToolbar", {available.x, toolbarHeight}, false, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(32, 32, 32, 255));
    auto sameIcon = [] { ImGui::SameLine(0.0f, 1.0f); };
    if (iconButton("undo", ICON_UNDO, "Undo", state.canUndo)) submitEdit({EditorActionType::UndoEditorEdit});
    sameIcon();
    if (iconButton("redo", ICON_REDO, "Redo", state.canRedo)) submitEdit({EditorActionType::RedoEditorEdit});
    sameIcon();
    auto const& selection = editor.selection();
    bool const canSplit = !project->sequence.empty() && (selection.getAs<editing::model::SelectedSequence>() || selection.getAs<editing::model::SelectedSequenceSegment>());
    if (iconButton("split", ICON_SPLIT, "Split at playhead", canSplit)) {
        EditorAction action{EditorActionType::SplitSequence};
        action.tick = state.currentTick;
        submitEdit(std::move(action));
    }
    sameIcon();
    bool const canDelete = selection.getAs<editing::model::SelectedSequenceSegment>()
        || (selection.getAs<editing::model::SelectedCamera>() && project->cameras.size() > 1)
        || selection.getAs<editing::model::SelectedKeyframe>();
    if (iconButton("delete", ICON_DELETE, "Delete selection", canDelete)) {
        if (auto const* sequenceSegmentSel = selection.getAs<editing::model::SelectedSequenceSegment>()) {
            EditorAction action{EditorActionType::DeleteSequenceSegment};
            action.id = sequenceSegmentSel->segmentId;
            submitEdit(std::move(action));
        } else if (auto const* cameraSel = selection.getAs<editing::model::SelectedCamera>()) {
            EditorAction action{EditorActionType::DeleteCamera};
            action.id = cameraSel->cameraId;
            submitEdit(std::move(action));
        } else if (auto const* keyframeSel = selection.getAs<editing::model::SelectedKeyframe>()) {
            EditorAction action{EditorActionType::DeleteCameraKeyframe};
            action.id = keyframeSel->trackId;
            action.secondaryId = keyframeSel->keyframeId;
            submitEdit(std::move(action));
        }
    }
    sameIcon();
    auto const* selectedCamera = selection.getAs<editing::model::SelectedCamera>();
    if (iconButton("add-key", ICON_ADD_KEYFRAME, "Add keyframe", selectedCamera != nullptr)) {
        EditorAction action{EditorActionType::AddCameraKeyframe};
        action.id   = selectedCamera->cameraId;
        action.tick = state.currentTick;
        submitEdit(std::move(action));
    }
    sameIcon();
    ImGui::PushStyleColor(ImGuiCol_CheckMark, IM_COL32(42, 147, 222, 255));
    ImGui::Checkbox("##timeline-snap", &mSnapEnabled);
    ImGui::PopStyleColor();
    sameIcon();
    if (iconButton("zoom-out", "-", "Zoom out")) mZoomScale = std::max(kMinZoomScale, mZoomScale / kZoomStep);
    sameIcon();
    float percent = mZoomScale * 100.0f;
    ImGui::SetNextItemWidth(82.0f);
    if (ImGui::DragFloat("##timeline-zoom", &percent, 1.0f, 100.0f, 2000.0f, "%.0f%%"))
        mZoomScale = std::clamp(percent / 100.0f, kMinZoomScale, kMaxZoomScale);
    sameIcon();
    if (iconButton("zoom-in", "+", "Zoom in")) mZoomScale = std::min(kMaxZoomScale, mZoomScale * kZoomStep);
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    float const workTop = fullMin.y + toolbarHeight;
    float const workBottom = fullMax.y - transportHeight;
    float const minimumListWidth = std::max(160.0f, fontSize * 12.0f);
    float const listWidth = std::clamp(available.x * mTrackListWidthRatio, minimumListWidth, available.x - std::max(180.0f, fontSize * 12.0f));
    float const canvasLeft = fullMin.x + listWidth + kSplitterThickness;
    float const canvasWidth = fullMax.x - canvasLeft;
    float const bodyTop = workTop + rulerHeight;
    float const bodyBottom = workBottom;
    float const fitPixelsPerTick = canvasWidth / static_cast<float>(std::max(1, state.totalTicks));
    float const pixelsPerTick = fitPixelsPerTick * mZoomScale;
    if (mZoomScale != zoomScaleBeforeToolbar) {
        float const previousPixelsPerTick = fitPixelsPerTick * zoomScaleBeforeToolbar;
        mScrollX = std::max(0.0f, mScrollX + displayTick * (pixelsPerTick - previousPixelsPerTick));
    }
    float const contentWidth = std::max(canvasWidth, state.totalTicks * pixelsPerTick);
    float const overflowWidth = std::max(0.0f, contentWidth - canvasWidth);
    bool const hasHorizontalOverflow = mZoomScale > kMinZoomScale + 0.001f && overflowWidth > 0.5f;
    float const maxScroll = hasHorizontalOverflow ? overflowWidth : 0.0f;
    mScrollX = std::clamp(mScrollX, 0.0f, maxScroll);
    float const contentHeight = std::max(0.0f, std::accumulate(mTrackTree.rows().begin(), mTrackTree.rows().end(), 0.0f, [](float value, auto const& row) {
        return value + row.height + 2.0f;
    }));
    float const visibleTrackHeight = std::max(0.0f, workBottom - bodyTop - 2.0f);
    mScrollY = std::clamp(mScrollY, 0.0f, std::max(0.0f, contentHeight - visibleTrackHeight));

    ImGui::SetCursorScreenPos({fullMin.x + listWidth - kSplitterThickness * 0.5f, workTop});
    ImGui::InvisibleButton("##timeline-list-splitter", {kSplitterThickness, workBottom - workTop});
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        mTrackListWidthRatio = std::clamp((ImGui::GetMousePos().x - fullMin.x) / available.x, 0.18f, 0.55f);
    }
    drawList->AddLine({fullMin.x + listWidth, workTop}, {fullMin.x + listWidth, fullMax.y}, kLine);

    drawList->AddRectFilled({fullMin.x, workTop}, {fullMin.x + listWidth, workBottom}, kSidebarBackground);
    ImGui::SetCursorScreenPos({fullMin.x, workTop});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild("##TimelineTrackControls", {listWidth, rulerHeight}, false, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPosY(std::max(0.0f, (rulerHeight - iconButtonSize()) * 0.5f));
    char search[128]{};
    std::snprintf(search, sizeof(search), "%s", mTrackSearch.c_str());
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(28, 122, 190, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(40, 142, 214, 255));
    if (ImGui::Button("+", {iconButtonSize(), iconButtonSize()})) ImGui::OpenPopup("##timeline-add-menu");
    if (ImGui::BeginPopup("##timeline-add-menu")) {
        if (project->sequence.empty() && ImGui::MenuItem("Add camera sequence")) {
            submitEdit({EditorActionType::AddCameraSequence});
        }
        if (ImGui::MenuItem("Add camera")) {
            EditorAction action{EditorActionType::AddFreeCamera};
            action.name = "playback.refactorEditor.defaults.camera"_tr(project->cameras.size() + 1);
            submitEdit(std::move(action));
        }
        ImGui::EndPopup();
    }
    sameIcon();
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0.0f, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
    ImGui::SetNextItemWidth(std::max(32.0f, ImGui::GetContentRegionAvail().x - 5.0f));
    if (ImGui::InputTextWithHint("##timeline-search", "Search Tracks", search, sizeof(search))) mTrackSearch = search;
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos({fullMin.x, bodyTop + 2.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild("##TimelineTrackList", {listWidth, workBottom - bodyTop - 2.0f}, false, ImGuiWindowFlags_NoScrollbar);
    float listY = bodyTop + 2.0f - mScrollY;
    for (auto const& row : mTrackTree.rows()) {
        float const rowBottom = listY + row.height;
        auto const* selectedKeyframe = editor.selection().getAs<editing::model::SelectedKeyframe>();
        bool selected = (row.kind == editing::model::TrackRowKind::Sequence && editor.selection().getAs<editing::model::SelectedSequence>())
            || (row.kind == editing::model::TrackRowKind::Camera && ((editor.selection().getAs<editing::model::SelectedCamera>() && editor.selection().getAs<editing::model::SelectedCamera>()->cameraId == row.id.substr(7)) || (selectedKeyframe && selectedKeyframe->trackId == row.id.substr(7))));
        ImGui::SetCursorScreenPos({fullMin.x, listY});
        ImGui::InvisibleButton(("##track-row-" + row.id).c_str(), {listWidth, row.height});
        bool const hovered = ImGui::IsItemHovered();
        bool const clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        if (selected) drawList->AddRect({fullMin.x + 1.0f, listY}, {fullMin.x + listWidth - 1.0f, rowBottom}, IM_COL32(122, 142, 166, 255), 0.0f, 0, 1.0f);
        else if (hovered) drawList->AddRectFilled({fullMin.x, listY}, {fullMin.x + listWidth, rowBottom}, IM_COL32(48, 48, 48, 255));
        float const textY = listY + (row.height - ImGui::GetFontSize()) * 0.5f;
        std::string label;
        if (row.kind == editing::model::TrackRowKind::Camera) {
            label = row.name;
            if (clicked) {
                auto const cameraId = row.id.substr(7);
                editor.selection().select(editing::model::SelectedCamera{cameraId});
                EditorAction action{EditorActionType::SetPreviewCamera};
                action.id = cameraId;
                editor.submitAction(std::move(action));
            }
        } else if (row.kind == editing::model::TrackRowKind::Sequence) {
            label = "O  Camera Sequence";
            if (clicked) {
                editor.selection().select(editing::model::SelectedSequence{});
                editor.submitAction({EditorActionType::ClearPreviewCamera});
            }
        }
        drawList->AddText({fullMin.x + 12.0f, textY}, IM_COL32(210, 210, 210, 255), label.c_str());
        if (row.locked) drawList->AddText({fullMin.x + listWidth - 48.0f, textY}, IM_COL32(140, 140, 140, 255), "LOCK");
        listY = rowBottom + 2.0f;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    drawList->AddRectFilled({canvasLeft, workTop}, {fullMax.x, workBottom}, kBackground);
    drawList->AddRectFilled({canvasLeft, workTop}, {fullMax.x, bodyTop}, kRulerBackground);
    drawList->AddLine({canvasLeft, bodyTop - 1.0f}, {fullMax.x, bodyTop - 1.0f}, kLine);
    drawList->AddRectFilled({fullMin.x, workBottom}, {canvasLeft, fullMax.y}, kSidebarBackground);
    drawList->AddLine({fullMin.x, workBottom}, {fullMax.x, workBottom}, kLine);
    drawList->PushClipRect({canvasLeft, workTop}, {fullMax.x, workBottom}, true);
    std::string const longestRulerLabel = formatTick(state.totalTicks);
    float const minimumMajorSpacing = std::max(90.0f, ImGui::CalcTextSize(longestRulerLabel.c_str()).x + 24.0f);
    int const majorStep = majorTickStep(pixelsPerTick, minimumMajorSpacing);
    int const minorStep = std::max(1, majorStep / 5);
    int const firstTick = std::max(0, static_cast<int>(std::floor(mScrollX / pixelsPerTick / minorStep)) * minorStep);
    float const rulerBaseline = bodyTop - 2.0f;
    for (int tick = firstTick; tick <= state.totalTicks; tick += minorStep) {
        float x = canvasLeft + tick * pixelsPerTick - mScrollX;
        if (x < canvasLeft || x > fullMax.x) continue;
        bool const major = tick % majorStep == 0;
        float const tickHeight = major ? 9.0f : 5.0f;
        drawList->AddLine(
            {x, rulerBaseline - tickHeight},
            {x, rulerBaseline},
            major ? IM_COL32(155, 158, 168, 255) : IM_COL32(82, 85, 94, 255)
        );
        if (major) {
            std::string const label = formatTick(tick);
            float const labelWidth = ImGui::CalcTextSize(label.c_str()).x;
            float const labelX = std::clamp(
                x - labelWidth * 0.5f,
                canvasLeft + 6.0f,
                fullMax.x - labelWidth - 6.0f
            );
            drawList->AddText({labelX, workTop + 3.0f}, IM_COL32(205, 208, 216, 255), label.c_str());
        }
    }

    ImGui::SetCursorScreenPos({canvasLeft, workTop});
    ImGui::InvisibleButton("##timeline-ruler", {canvasWidth, rulerHeight});
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        displayTick = std::clamp(static_cast<int>((ImGui::GetMousePos().x - canvasLeft + mScrollX) / pixelsPerTick), 0, state.totalTicks);
    }
    if (ImGui::IsItemDeactivated()) submitSeek(displayTick);

    auto segmentLabel = [&project](editing::model::SequenceSegment const& segment) -> char const* {
        if (segment.cameraId.empty()) return project->cameras.empty() ? "No camera" : "Auto (first camera)";
        auto it = std::find_if(project->cameras.begin(), project->cameras.end(), [&segment](auto const& camera) { return camera.id == segment.cameraId; });
        return it == project->cameras.end() ? "Missing camera" : it->name.c_str();
    };
    auto tickFromMouse = [&] { return std::clamp(static_cast<int>((ImGui::GetMousePos().x - canvasLeft + mScrollX) / pixelsPerTick), 0, state.totalTicks); };
    auto const* selectedKeyframe = editor.selection().getAs<editing::model::SelectedKeyframe>();
    float y = bodyTop + 2.0f - mScrollY;
    bool clickConsumed = false;
    for (auto const& row : mTrackTree.rows()) {
        float const rowBottom = y + row.height;
        bool const rowSelected = (row.kind == editing::model::TrackRowKind::Sequence
                                  && (editor.selection().getAs<editing::model::SelectedSequence>() || editor.selection().getAs<editing::model::SelectedSequenceSegment>()))
            || (row.kind == editing::model::TrackRowKind::Camera
                && ((editor.selection().getAs<editing::model::SelectedCamera>() && editor.selection().getAs<editing::model::SelectedCamera>()->cameraId == row.id.substr(7))
                    || (selectedKeyframe && selectedKeyframe->trackId == row.id.substr(7))));
        drawList->AddRectFilled({canvasLeft, y}, {fullMax.x, rowBottom}, IM_COL32(29, 29, 29, 255));
        if (row.kind == editing::model::TrackRowKind::Camera && row.cameraIndex >= 0
            && row.cameraIndex < static_cast<int>(project->cameras.size())
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && ImGui::GetMousePos().x >= canvasLeft && ImGui::GetMousePos().x <= fullMax.x
            && ImGui::GetMousePos().y >= y && ImGui::GetMousePos().y <= rowBottom) {
            clickConsumed = true;
            auto const& camera = project->cameras[row.cameraIndex];
            editor.selection().select(editing::model::SelectedCamera{camera.id});
            EditorAction action{EditorActionType::SetPreviewCamera};
            action.id = camera.id;
            editor.submitAction(std::move(action));
        }
        if (row.kind == editing::model::TrackRowKind::Sequence) {
            for (auto const& segment : project->sequence) {
                ImVec2 minimum{canvasLeft + segment.startTick * pixelsPerTick - mScrollX, y + 4.0f};
                ImVec2 maximum{canvasLeft + segment.endTick * pixelsPerTick - mScrollX, rowBottom - 4.0f};
                bool selected = editor.selection().getAs<editing::model::SelectedSequenceSegment>() && editor.selection().getAs<editing::model::SelectedSequenceSegment>()->segmentId == segment.id;
                drawList->AddRectFilled(minimum, maximum, selected ? IM_COL32(139, 155, 178, 255) : kSequenceColor);
                drawList->AddRect(minimum, maximum, selected ? IM_COL32(220, 220, 220, 255) : IM_COL32(178, 178, 178, 255));
                drawList->AddText({minimum.x + 5.0f, minimum.y + 6.0f}, IM_COL32(245, 245, 247, 255), segmentLabel(segment));
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && contains(minimum, maximum, ImGui::GetMousePos())) {
                    clickConsumed = true;
                    editor.selection().select(editing::model::SelectedSequenceSegment{segment.id});
                    if (!segment.locked && (std::abs(ImGui::GetMousePos().x - minimum.x) < 8.0f || std::abs(ImGui::GetMousePos().x - maximum.x) < 8.0f)) {
                        mDraggingSegmentId = segment.id;
                        mDraggingStart = std::abs(ImGui::GetMousePos().x - minimum.x) < std::abs(ImGui::GetMousePos().x - maximum.x);
                        mDragStartTick = segment.startTick;
                        mDragEndTick   = segment.endTick;
                    }
                }
            }
        } else if (row.kind == editing::model::TrackRowKind::Camera && row.cameraIndex >= 0
                   && row.cameraIndex < static_cast<int>(project->cameras.size())) {
            auto const& camera = project->cameras[row.cameraIndex];
            drawList->AddRectFilled({canvasLeft, y + 5.0f}, {fullMax.x, rowBottom - 5.0f}, rowSelected ? IM_COL32(112, 87, 124, 255) : kCameraColor);
            for (auto const& key : camera.keys) {
                float x = canvasLeft + key.tick * pixelsPerTick - mScrollX;
                float const centerY = (y + rowBottom) * 0.5f;
                bool const selected = selectedKeyframe && selectedKeyframe->trackId == camera.id && selectedKeyframe->keyframeId == key.id;
                ImVec2 const top{x, centerY - 5.0f};
                ImVec2 const right{x + 5.0f, centerY};
                ImVec2 const bottom{x, centerY + 5.0f};
                ImVec2 const left{x - 5.0f, centerY};
                drawList->AddQuadFilled(top, right, bottom, left, selected ? IM_COL32(244, 202, 47, 255) : IM_COL32(255, 255, 255, 255));
                drawList->AddQuad(top, right, bottom, left, IM_COL32(24, 24, 24, 255), 1.0f);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && std::abs(ImGui::GetMousePos().x - x) <= 7.0f && ImGui::GetMousePos().y >= y && ImGui::GetMousePos().y <= rowBottom) {
                    clickConsumed = true;
                    editor.selection().select(editing::model::SelectedKeyframe{camera.id, key.id});
                    EditorAction previewAction{EditorActionType::SetPreviewCamera};
                    previewAction.id = camera.id;
                    editor.submitAction(std::move(previewAction));
                    submitSeek(key.tick);
                }
            }
        }
        y = rowBottom + 2.0f;
    }

    float const playheadX = std::clamp(canvasLeft + displayTick * pixelsPerTick - mScrollX, canvasLeft, fullMax.x);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && std::abs(ImGui::GetMousePos().x - playheadX) <= 6.0f
        && ImGui::GetMousePos().y >= workTop && ImGui::GetMousePos().y < workBottom) {
        mDraggingPlayhead = true;
        clickConsumed = true;
    }
    if (mDraggingPlayhead) {
        displayTick = tickFromMouse();
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            submitSeek(displayTick);
            mDraggingPlayhead = false;
        }
    }
    if (!clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && ImGui::GetMousePos().x >= canvasLeft && ImGui::GetMousePos().x <= fullMax.x
        && ImGui::GetMousePos().y >= bodyTop && ImGui::GetMousePos().y < workBottom - (maxScroll > 0.0f ? 18.0f : 0.0f)) {
        submitSeek(tickFromMouse());
    }

    if (!mDraggingSegmentId.empty()) {
        int tick = tickFromMouse();
        if (mSnapEnabled) tick = std::clamp(static_cast<int>(std::round(tick / 20.0f)) * 20, 0, state.totalTicks);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float x = canvasLeft + tick * pixelsPerTick - mScrollX;
            drawList->AddLine({x, bodyTop}, {x, bodyBottom}, IM_COL32(240, 192, 32, 180), 2.0f);
        } else {
            EditorAction action{EditorActionType::TrimSequence};
            action.id = mDraggingSegmentId;
            action.tick = mDraggingStart ? tick : mDragStartTick;
            action.kind = mDraggingStart ? mDragEndTick : tick;
            submitEdit(std::move(action));
            mDraggingSegmentId.clear();
        }
    }

    float const visiblePlayheadX = std::clamp(
        canvasLeft + displayTick * pixelsPerTick - mScrollX,
        canvasLeft + 5.0f,
        fullMax.x - 5.0f
    );
    drawList->AddLine({visiblePlayheadX, bodyTop - 2.0f}, {visiblePlayheadX, bodyBottom}, kPlayheadColor, 1.5f);
    drawList->AddTriangleFilled(
        {visiblePlayheadX - 5.0f, bodyTop - 11.0f},
        {visiblePlayheadX + 5.0f, bodyTop - 11.0f},
        {visiblePlayheadX, bodyTop - 2.0f},
        kPlayheadColor
    );
    drawList->PopClipRect();
    ImVec2 const mouse = ImGui::GetMousePos();
    bool const canvasHovered = mouse.x >= canvasLeft && mouse.x <= fullMax.x && mouse.y >= workTop && mouse.y < workBottom;
    bool const trackListHovered = mouse.x >= fullMin.x && mouse.x < canvasLeft && mouse.y >= bodyTop && mouse.y < workBottom;
    if ((canvasHovered || trackListHovered) && ImGui::GetIO().MouseWheel != 0.0f) {
        float const wheel = ImGui::GetIO().MouseWheel;
        if (trackListHovered && !ImGui::GetIO().KeyShift) {
            mScrollY = std::clamp(mScrollY - wheel * 45.0f, 0.0f, std::max(0.0f, contentHeight - visibleTrackHeight));
        } else if (ImGui::GetIO().KeyShift) {
            float const anchorX = std::clamp(ImGui::GetMousePos().x - canvasLeft, 0.0f, canvasWidth);
            float const anchorTick = (anchorX + mScrollX) / pixelsPerTick;
            mZoomScale = std::clamp(mZoomScale * (wheel > 0.0f ? kZoomStep : 1.0f / kZoomStep), kMinZoomScale, kMaxZoomScale);
            float const nextPixelsPerTick = fitPixelsPerTick * mZoomScale;
            float const nextMaxScroll = std::max(0.0f, state.totalTicks * nextPixelsPerTick - canvasWidth);
            mScrollX = std::clamp(anchorTick * nextPixelsPerTick - anchorX, 0.0f, nextMaxScroll);
        } else if (maxScroll > 0.0f) {
            mScrollX = std::clamp(mScrollX - wheel * 60.0f, 0.0f, maxScroll);
        } else {
            mScrollY = std::clamp(mScrollY - wheel * 45.0f, 0.0f, std::max(0.0f, contentHeight - visibleTrackHeight));
        }
    }

    float const scrollbarHeight = ImGui::GetFrameHeight();
    float const scrollbarY = workBottom + std::max(0.0f, (transportHeight - scrollbarHeight) * 0.5f);
    float const scrollbarHorizontalPadding = std::min(8.0f, canvasWidth * 0.25f);
    float const scrollbarWidth = std::max(1.0f, canvasWidth - scrollbarHorizontalPadding * 2.0f);
    ImGui::SetCursorScreenPos({canvasLeft + scrollbarHorizontalPadding, scrollbarY});
    if (maxScroll > 0.0f) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(54, 54, 54, 255));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(137, 137, 137, 255));
        ImGui::SetNextItemWidth(scrollbarWidth);
        ImGui::SliderFloat("##timeline-scroll", &mScrollX, 0.0f, maxScroll, "", ImGuiSliderFlags_NoInput);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
    }
    ImGui::SetCursorScreenPos({fullMin.x, workBottom});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild("##TimelineTransport", {listWidth, transportHeight}, false, ImGuiWindowFlags_NoScrollbar);
    float const buttonSize = iconButtonSize();
    float const controlsWidth = buttonSize * 5.0f + 4.0f;
    float const transportContentWidth = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(std::max(0.0f, (transportContentWidth - controlsWidth) * 0.5f));
    ImGui::SetCursorPosY(std::max(0.0f, (transportHeight - buttonSize) * 0.5f));
    if (iconButton("transport-start", ICON_SKIP_BACK, "Skip to start")) submitEdit({EditorActionType::SkipToStart});
    sameIcon();
    if (iconButton("transport-prev", ICON_CHEVRONS_LEFT, "Previous frame")) { EditorAction action{EditorActionType::Seek}; action.tick = std::max(0, state.currentTick - 1); submitEdit(std::move(action)); }
    sameIcon();
    if (iconButton("transport-play", state.paused ? ICON_PLAY : ICON_PAUSE, state.paused ? "Play" : "Pause")) submitEdit({EditorActionType::TogglePause});
    sameIcon();
    if (iconButton("transport-next", ICON_CHEVRONS_RIGHT, "Next frame")) { EditorAction action{EditorActionType::Seek}; action.tick = std::min(state.totalTicks, state.currentTick + 1); submitEdit(std::move(action)); }
    sameIcon();
    if (iconButton("transport-end", ICON_SKIP_FORWARD, "Skip to end")) submitEdit({EditorActionType::SkipToEnd});
    sameIcon();
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

} // namespace playback::editor::ui
