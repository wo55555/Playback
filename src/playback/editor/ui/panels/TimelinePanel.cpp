#include "TimelinePanel.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/TimelineScale.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/editor/ui/iconfont.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace playback::editor::ui {
using namespace playback::state;

using namespace ll::i18n_literals;

namespace {

constexpr float kSplitterThickness = 4.0f;
constexpr float kMinZoomScale      = 1.0f;
constexpr float kMaxZoomScale      = 20.0f;
constexpr float kZoomStep          = 1.15f;
constexpr int   kTicksPerSecond    = 20;

constexpr ImU32 kBackground        = theme::kTimelineBg;
constexpr ImU32 kSidebarBackground = theme::kTimelineSidebar;
constexpr ImU32 kRulerBackground   = theme::kTimelineRuler;
constexpr ImU32 kLine              = theme::kLine;
constexpr ImU32 kCameraColor       = theme::kCameraColor;
constexpr ImU32 kCameraSelected    = theme::kCameraSelected;
constexpr ImU32 kPlayheadColor     = theme::kAccent;
constexpr ImU32 kRowHighlight      = theme::kRowHighlight;
constexpr ImU32 kRowSelectedBorder = theme::kRowSelectedBorder;

using widgets::formatTick;
using widgets::iconButton;
using widgets::iconButtonSize;

int majorTickStep(float pixelsPerTick, float minimumSpacing) {
    constexpr int steps[] = {
        kTicksPerSecond,
        kTicksPerSecond * 2,
        kTicksPerSecond * 5,
        kTicksPerSecond * 10,
        kTicksPerSecond * 20,
        kTicksPerSecond * 30,
        kTicksPerSecond * 60,
        kTicksPerSecond * 120,
        kTicksPerSecond * 300,
        kTicksPerSecond * 600,
    };
    for (int step : steps)
        if (static_cast<float>(step) * pixelsPerTick >= minimumSpacing) return step;
    return steps[std::size(steps) - 1];
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
    mRulerDragTick       = -1;
    mAnimator.clear();
    mDraggingKeyframeCameraId.clear();
    mDraggingKeyframeStartTick = -1;
    mDraggingKeyframeMoved     = false;
}

void TimelinePanel::submitSeek(PanelContext const& ctx, int tick) {
    mPendingSeekTick = std::clamp(tick, 0, std::max(0, ctx.state.totalTicks));
    EditorAction action{EditorActionType::Seek};
    action.tick = mPendingSeekTick;
    ctx.submitAction(std::move(action));
}

void TimelinePanel::seekTo(PanelContext const& ctx, int tick) { submitSeek(ctx, tick); }

void TimelinePanel::seekRelative(PanelContext const& ctx, int tickDelta) {
    int const baseTick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    submitSeek(ctx, baseTick + tickDelta);
}

void TimelinePanel::seekAdjacentEditPoint(PanelContext const& ctx, bool forward) {
    auto const& state   = ctx.state;
    auto const  project = state.project;
    if (!project) return;

    int const  baseTick = mPendingSeekTick >= 0 ? mPendingSeekTick : state.currentTick;
    int        target   = forward ? state.totalTicks : 0;
    bool       found    = false;
    auto const consider = [&](int tick) {
        if ((forward && tick <= baseTick) || (!forward && tick >= baseTick)) return;
        if (!found || (forward ? tick < target : tick > target)) {
            target = tick;
            found  = true;
        }
    };

    for (auto const& segment : project->sequence) {
        consider(segment.startTick);
        consider(segment.endTick);
    }
    for (auto const& segment : project->worldActor.segments) {
        consider(segment.startTick);
        consider(segment.endTick);
    }
    for (auto const& camera : project->cameras) {
        for (auto const& [tick, _] : camera.keysByTick) consider(tick);
    }
    for (auto const& marker : project->markers) consider(marker.tick);
    submitSeek(ctx, target);
}

bool TimelinePanel::addKeyframeAtPlayhead(PanelContext const& ctx) {
    auto const* selectedCamera = ctx.selection.getAs<state::editing::model::SelectedCamera>();
    if (!selectedCamera) return false;

    EditorAction action{EditorActionType::AddCameraKeyframe};
    action.id   = selectedCamera->cameraId;
    action.tick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    ctx.submitAction(std::move(action));
    return true;
}

bool TimelinePanel::deleteSelection(PanelContext const& ctx) {
    auto const project = ctx.state.project;
    if (!project) return false;

    EditorAction action;
    if (auto const* selectedCamera = ctx.selection.getAs<state::editing::model::SelectedCamera>();
        selectedCamera && project->cameras.size() > 1) {
        action.type = EditorActionType::DeleteCamera;
        action.id   = selectedCamera->cameraId;
    } else if (auto const* selectedKeyframe = ctx.selection.getAs<state::editing::model::SelectedKeyframe>()) {
        action.type = EditorActionType::DeleteCameraKeyframe;
        action.id   = selectedKeyframe->trackId;
        action.tick = selectedKeyframe->tick;
    } else {
        return false;
    }

    ctx.submitAction(std::move(action));
    ctx.selection.clear();
    return true;
}

void TimelinePanel::zoomIn() { mZoomScale = std::min(kMaxZoomScale, mZoomScale * kZoomStep); }

void TimelinePanel::zoomOut() { mZoomScale = std::max(kMinZoomScale, mZoomScale / kZoomStep); }

void TimelinePanel::resetZoom() {
    mZoomScale = kMinZoomScale;
    mScrollX   = 0.0f;
}

void TimelinePanel::draw(PanelContext const& ctx, bool allowInput) {
    auto const& state   = ctx.state;
    auto const  project = state.project;
    if (!project) {
        widgets::noActiveProjectPlaceholder();
        return;
    }

    if (!allowInput) {
        mRulerDragTick    = -1;
        mDraggingPlayhead = false;
        mDraggingKeyframeCameraId.clear();
        mDraggingKeyframeStartTick = -1;
        mDraggingKeyframeMoved     = false;
    }

    mAnimator.beginFrame();
    mTrackTree.setSearch(mTrackSearch);
    mTrackTree.setCamerasExpanded(mCamerasExpanded);
    mTrackTree.rebuild(*project);
    int displayTick = mPendingSeekTick >= 0 ? mPendingSeekTick : state.currentTick;
    if (mRulerDragTick >= 0) displayTick = mRulerDragTick;
    if (mPendingSeekTick >= 0 && state.currentTick == mPendingSeekTick) mPendingSeekTick = -1;

    ImVec2 const fullMin   = ImGui::GetCursorScreenPos();
    ImVec2 const available = ImGui::GetContentRegionAvail();
    if (available.x < 220.0f || available.y < 120.0f) return;
    ImVec2 const fullMax{fullMin.x + available.x, fullMin.y + available.y};
    float const  fontSize        = ImGui::GetFontSize();
    float const  toolbarHeight   = fontSize + 16.0f;
    float const  transportHeight = iconButtonSize() + 8.0f;
    float const  rulerHeight     = fontSize + 18.0f;
    auto*        drawList        = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(fullMin, fullMax, kBackground);
    float const zoomScaleBeforeToolbar = mZoomScale;

    ImGui::SetCursorScreenPos(fullMin);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild("##TimelineToolbar", {available.x, toolbarHeight}, false, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(32, 32, 32, 255));
    auto        sameIcon  = [] { ImGui::SameLine(0.0f, 1.0f); };
    auto        nextGroup = [] { ImGui::SameLine(0.0f, 8.0f); };
    auto const& selection = ctx.selection;
    bool const  canDelete = (selection.getAs<state::editing::model::SelectedCamera>() && project->cameras.size() > 1)
                         || selection.getAs<state::editing::model::SelectedKeyframe>();
    auto const  deleteTooltip = "playback.refactorEditor.timeline.deleteSelection"_tr();
    if (iconButton("delete", ICON_DELETE, deleteTooltip.c_str(), canDelete)) (void)deleteSelection(ctx);
    sameIcon();
    auto const* selectedCamera = selection.getAs<state::editing::model::SelectedCamera>();
    auto const  addKeyTooltip  = "playback.refactorEditor.timeline.addKeyframe"_tr();
    if (iconButton("add-key", ICON_ADD_KEYFRAME, addKeyTooltip.c_str(), selectedCamera != nullptr))
        (void)addKeyframeAtPlayhead(ctx);
    nextGroup();
    ImGui::PushStyleColor(ImGuiCol_CheckMark, IM_COL32(42, 147, 222, 255));
    ImGui::Checkbox(("playback.refactorEditor.timeline.snap"_tr() + "##timeline-snap").c_str(), &mSnapEnabled);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", "playback.refactorEditor.timeline.snapTooltip"_tr().c_str());
    }
    nextGroup();
    auto const zoomOutTooltip   = "playback.refactorEditor.timeline.zoomOut"_tr();
    auto const zoomInTooltip    = "playback.refactorEditor.timeline.zoomIn"_tr();
    auto const zoomResetTooltip = "playback.refactorEditor.timeline.zoomReset"_tr();
    if (iconButton("zoom-out", "-", zoomOutTooltip.c_str())) zoomOut();
    sameIcon();
    float percent = mZoomScale * 100.0f;
    ImGui::SetNextItemWidth(82.0f);
    if (ImGui::DragFloat("##timeline-zoom", &percent, 1.0f, 100.0f, 2000.0f, "%.0f%%"))
        mZoomScale = std::clamp(percent / 100.0f, kMinZoomScale, kMaxZoomScale);
    sameIcon();
    if (iconButton("zoom-in", "+", zoomInTooltip.c_str())) zoomIn();
    sameIcon();
    if (iconButton("zoom-reset", ICON_RESET, zoomResetTooltip.c_str())) resetZoom();
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    float const scrollbarLaneHeight = ImGui::GetFrameHeight() + 4.0f;
    float const workTop             = fullMin.y + toolbarHeight;
    float const workBottom          = fullMax.y - transportHeight - scrollbarLaneHeight;
    float const minimumListWidth    = std::max(160.0f, fontSize * 12.0f);
    float const listWidth           = std::clamp(
        available.x * mTrackListWidthRatio,
        minimumListWidth,
        available.x - std::max(180.0f, fontSize * 12.0f)
    );
    float const canvasLeft       = fullMin.x + listWidth + kSplitterThickness;
    float const canvasWidth      = fullMax.x - canvasLeft;
    float const bodyTop          = workTop + rulerHeight;
    float const bodyBottom       = workBottom;
    float const fitPixelsPerTick = canvasWidth / static_cast<float>(std::max(1, state.totalTicks));
    float const pixelsPerTick    = fitPixelsPerTick * mZoomScale;
    if (mZoomScale != zoomScaleBeforeToolbar) {
        float const previousPixelsPerTick = fitPixelsPerTick * zoomScaleBeforeToolbar;
        mScrollX = std::max(0.0f, mScrollX + displayTick * (pixelsPerTick - previousPixelsPerTick));
    }
    float const contentWidth          = std::max(canvasWidth, state.totalTicks * pixelsPerTick);
    float const overflowWidth         = std::max(0.0f, contentWidth - canvasWidth);
    bool const  hasHorizontalOverflow = mZoomScale > kMinZoomScale + 0.001f && overflowWidth > 0.5f;
    float const maxScroll             = hasHorizontalOverflow ? overflowWidth : 0.0f;
    mScrollX                          = std::clamp(mScrollX, 0.0f, maxScroll);

    TimelineScale const scale{canvasLeft, canvasWidth, pixelsPerTick, mScrollX, state.totalTicks};

    float trackContentHeight = 0.0f;
    for (auto const& row : mTrackTree.rows()) trackContentHeight += row.height + 2.0f;
    float const visibleTrackHeight = std::max(0.0f, workBottom - bodyTop - 2.0f);
    float const maxScrollY         = std::max(0.0f, trackContentHeight - visibleTrackHeight);
    mScrollY                       = std::clamp(mScrollY, 0.0f, maxScrollY);

    ImGui::SetCursorScreenPos({fullMin.x + listWidth - kSplitterThickness * 0.5f, workTop});
    ImGui::InvisibleButton("##timeline-list-splitter", {kSplitterThickness, workBottom - workTop});
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (allowInput && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
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
    if (ImGui::Button("+##timeline-add", {iconButtonSize(), iconButtonSize()})) {
        ImGui::OpenPopup("##timeline-add-menu");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "playback.refactorEditor.timeline.addTrack"_tr().c_str());
    if (ImGui::BeginPopup("##timeline-add-menu")) {
        if (ImGui::MenuItem("playback.refactorEditor.details.addFreeCamera"_tr().c_str())) {
            EditorAction action{EditorActionType::AddFreeCamera};
            action.name = "playback.refactorEditor.defaults.camera"_tr(project->cameras.size() + 1);
            ctx.submitAction(std::move(action));
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
    ImGui::SetNextItemWidth(std::max(32.0f, ImGui::GetContentRegionAvail().x - 5.0f));
    auto const searchHint = "playback.refactorEditor.timeline.searchTracks"_tr();
    if (ImGui::InputTextWithHint("##timeline-search", searchHint.c_str(), search, sizeof(search))) {
        mTrackSearch = search;
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos({fullMin.x, bodyTop + 2.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild(
        "##TimelineTrackList",
        {listWidth, workBottom - bodyTop - 2.0f},
        false,
        ImGuiWindowFlags_NoScrollbar
    );
    drawList->PushClipRect({fullMin.x, bodyTop + 2.0f}, {fullMin.x + listWidth, workBottom}, true);
    float listY = bodyTop + 2.0f - mScrollY;
    for (auto const& row : mTrackTree.rows()) {
        float const rowBottom        = listY + row.height;
        auto const* selectedKeyframe = ctx.selection.getAs<state::editing::model::SelectedKeyframe>();
        bool selected                = row.kind == TrackRowKind::Camera
                                    && ((ctx.selection.getAs<state::editing::model::SelectedCamera>()
                                         && ctx.selection.getAs<state::editing::model::SelectedCamera>()->cameraId == row.id.substr(7))
                                        || (selectedKeyframe && selectedKeyframe->trackId == row.id.substr(7)));
        if (rowBottom < bodyTop || listY > workBottom) {
            listY = rowBottom + 2.0f;
            continue;
        }
        ImGui::SetCursorScreenPos({fullMin.x, listY});
        ImGui::InvisibleButton(("##track-row-" + row.id).c_str(), {listWidth, row.height});
        bool const  hovered        = ImGui::IsItemHovered();
        bool const  clicked        = allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Left);
        float const selectedAmount = mAnimator.animate("row-selected", row.id, selected ? 1.0f : 0.0f);
        float const hoverAmount    = mAnimator.animate("row-hover", row.id, hovered ? 1.0f : 0.0f);
        float const highlight      = std::max(selectedAmount, hoverAmount);
        if (highlight > 0.0f) {
            drawList->AddRectFilled(
                {fullMin.x, listY},
                {fullMin.x + listWidth, rowBottom},
                lerpColor(kSidebarBackground, kRowHighlight, highlight)
            );
        }
        if (selectedAmount > 0.0f) {
            drawList->AddRect(
                {fullMin.x + 1.0f, listY},
                {fullMin.x + listWidth - 1.0f, rowBottom},
                lerpColor(IM_COL32(122, 142, 166, 0), kRowSelectedBorder, selectedAmount),
                0.0f,
                0,
                1.0f
            );
            drawList->AddRectFilled(
                {fullMin.x, listY},
                {fullMin.x + 3.0f, rowBottom},
                lerpColor(IM_COL32(58, 140, 240, 0), kPlayheadColor, selectedAmount)
            );
        }
        float const textY = listY + (row.height - ImGui::GetFontSize()) * 0.5f;
        std::string label;
        if (row.kind == TrackRowKind::Camera) {
            label = std::string(ICON_CAMERA) + "  " + row.name;
            if (!row.enabled) {
                drawList->AddText({fullMin.x + 12.0f, textY}, IM_COL32(118, 118, 118, 255), label.c_str());
            }
            float const  controlSize = std::max(18.0f, ImGui::GetFontSize() + 4.0f);
            float const  enabledX    = fullMin.x + listWidth - controlSize - 8.0f;
            float const  controlY    = listY + (row.height - controlSize) * 0.5f;
            ImVec2 const mouse       = ImGui::GetMousePos();
            bool const   enabledHovered =
                contains({enabledX, controlY}, {enabledX + controlSize, controlY + controlSize}, mouse);
            if (enabledHovered) {
                ImGui::SetTooltip(
                    "%s",
                    (row.enabled ? "playback.refactorEditor.timeline.disableTrack"_tr()
                                 : "playback.refactorEditor.timeline.enableTrack"_tr())
                        .c_str()
                );
            }
            if (allowInput && enabledHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                EditorAction action{EditorActionType::SetCameraEnabled};
                action.id    = row.id.substr(7);
                action.value = !row.enabled;
                ctx.submitAction(std::move(action));
            }
            drawList->AddText(
                {enabledX + (controlSize - ImGui::CalcTextSize(row.enabled ? ICON_EYE : ICON_HIDE).x) * 0.5f, textY},
                enabledHovered ? IM_COL32(245, 245, 245, 255) : IM_COL32(165, 165, 165, 255),
                row.enabled ? ICON_EYE : ICON_HIDE
            );

            bool const clickOnControls =
                ImGui::GetMousePos().x >= enabledX && ImGui::GetMousePos().x <= enabledX + controlSize;
            if (clicked && !clickOnControls) {
                auto const cameraId = row.id.substr(7);
                ctx.selection.select(state::editing::model::SelectedCamera{cameraId});
                EditorAction action{EditorActionType::SetPreviewCamera};
                action.id = cameraId;
                ctx.submitAction(std::move(action));
            }
        }
        if (row.kind == TrackRowKind::Camera && row.enabled) {
            drawList->AddText({fullMin.x + 12.0f, textY}, IM_COL32(210, 210, 210, 255), label.c_str());
        }
        if (row.locked) {
            float const lockX = fullMin.x + listWidth - 48.0f;
            drawList->AddText({lockX, textY}, IM_COL32(140, 140, 140, 255), ICON_LOCK);
        }
        listY = rowBottom + 2.0f;
    }
    drawList->PopClipRect();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    drawList->AddRectFilled({canvasLeft, workTop}, {fullMax.x, workBottom}, kBackground);
    drawList->AddRectFilled({canvasLeft, workTop}, {fullMax.x, bodyTop}, kRulerBackground);
    drawList->AddLine({canvasLeft, bodyTop - 1.0f}, {fullMax.x, bodyTop - 1.0f}, kLine);
    drawList->AddRectFilled({fullMin.x, workBottom}, {fullMax.x, fullMax.y}, kSidebarBackground);
    drawList->AddLine({fullMin.x, workBottom}, {fullMax.x, workBottom}, kLine);
    if (maxScrollY > 0.0f) {
        float const trackHeight = std::max(1.0f, visibleTrackHeight);
        float const thumbHeight = std::max(24.0f, trackHeight * trackHeight / std::max(1.0f, trackContentHeight));
        float const thumbTop    = bodyTop + 2.0f + (trackHeight - thumbHeight) * (mScrollY / maxScrollY);
        drawList->AddRectFilled(
            {fullMax.x - 6.0f, thumbTop},
            {fullMax.x - 2.0f, thumbTop + thumbHeight},
            IM_COL32(137, 137, 137, 200),
            2.0f
        );
    }
    drawList->PushClipRect({canvasLeft, workTop}, {fullMax.x, workBottom}, true);
    std::string const longestRulerLabel   = formatTick(state.totalTicks);
    float const       minimumMajorSpacing = std::max(90.0f, ImGui::CalcTextSize(longestRulerLabel.c_str()).x + 24.0f);
    int const         majorStep           = majorTickStep(pixelsPerTick, minimumMajorSpacing);
    int const         minorStep           = std::max(1, majorStep / 5);
    int const         firstTick           = scale.firstVisibleTick(minorStep);
    float const       rulerBaseline       = bodyTop - 2.0f;
    float             lastLabelRight      = canvasLeft - 6.0f;
    for (int tick = firstTick; tick <= state.totalTicks; tick += minorStep) {
        float x = scale.xAt(static_cast<float>(tick));
        if (x < canvasLeft || x > fullMax.x) continue;
        bool const  major      = tick % majorStep == 0;
        float const tickHeight = major ? 9.0f : 5.0f;
        drawList->AddLine(
            {x, rulerBaseline - tickHeight},
            {x, rulerBaseline},
            major ? IM_COL32(155, 158, 168, 255) : IM_COL32(82, 85, 94, 255)
        );
        if (major) {
            std::string const label      = formatTick(tick);
            float const       labelWidth = ImGui::CalcTextSize(label.c_str()).x;
            float const       labelX     = x - labelWidth * 0.5f;
            // Skip instead of clamping into place; clamping stacks labels on top of each other at the edges.
            if (labelX >= lastLabelRight + 8.0f && labelX + labelWidth <= fullMax.x - 6.0f) {
                drawList->AddText({labelX, workTop + 3.0f}, IM_COL32(205, 208, 216, 255), label.c_str());
                lastLabelRight = labelX + labelWidth;
            }
        }
    }

    ImGui::SetCursorScreenPos({canvasLeft, workTop});
    ImGui::InvisibleButton("##timeline-ruler", {canvasWidth, rulerHeight});
    if (allowInput && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        mRulerDragTick = std::clamp(scale.tickAt(ImGui::GetMousePos().x), 0, state.totalTicks);
        displayTick    = mRulerDragTick;
    }
    if (allowInput && ImGui::IsItemDeactivated()) {
        if (mRulerDragTick >= 0) submitSeek(ctx, mRulerDragTick);
        mRulerDragTick = -1;
    }

    auto tickFromMouse = [&] { return std::clamp(scale.tickAt(ImGui::GetMousePos().x), 0, state.totalTicks); };
    auto snapTick      = [&](int tick) {
        tick = std::clamp(tick, 0, state.totalTicks);
        if (!mSnapEnabled) return tick;
        constexpr float GridTicks = static_cast<float>(kTicksPerSecond);
        return std::clamp(
            static_cast<int>(std::round(static_cast<float>(tick) / GridTicks) * GridTicks),
            0,
            state.totalTicks
        );
    };
    auto boundedKeyframeTick = [&](int tick) {
        auto const camera =
            std::ranges::find(project->cameras, mDraggingKeyframeCameraId, &state::editing::model::CameraEntity::id);
        if (camera == project->cameras.end()) return tick;
        auto const key = camera->keysByTick.find(mDraggingKeyframeStartTick);
        if (key == camera->keysByTick.end()) return tick;

        int minimum = 0;
        int maximum = state.totalTicks;
        if (key != camera->keysByTick.begin()) minimum = std::min(state.totalTicks, std::prev(key)->first + 1);
        auto const next = std::next(key);
        if (next != camera->keysByTick.end()) maximum = std::max(0, next->first - 1);
        if (minimum > maximum) return mDraggingKeyframeStartTick;
        return std::clamp(tick, minimum, maximum);
    };
    auto const* selectedKeyframe = ctx.selection.getAs<state::editing::model::SelectedKeyframe>();
    float       y                = bodyTop + 2.0f - mScrollY;
    bool        clickConsumed    = false;
    for (auto const& row : mTrackTree.rows()) {
        float const rowBottom = y + row.height;
        if (rowBottom < bodyTop || y > workBottom) {
            y = rowBottom + 2.0f;
            continue;
        }
        drawList->AddRectFilled({canvasLeft, y}, {fullMax.x, rowBottom}, theme::kTimelineTrackBg);
        if (row.kind == TrackRowKind::Camera && row.cameraIndex >= 0
            && row.cameraIndex < static_cast<int>(project->cameras.size())) {
            auto const& camera = project->cameras[row.cameraIndex];
            bool const  cameraSelected =
                (ctx.selection.getAs<state::editing::model::SelectedCamera>()
                 && ctx.selection.getAs<state::editing::model::SelectedCamera>()->cameraId == camera.id)
                || (selectedKeyframe && selectedKeyframe->trackId == camera.id);
            float const cameraAmount = mAnimator.animate("canvas-selected", camera.id, cameraSelected ? 1.0f : 0.0f);
            ImU32 const cameraColor =
                camera.enabled ? lerpColor(kCameraColor, kCameraSelected, cameraAmount) : IM_COL32(56, 52, 58, 180);
            drawList->AddRectFilled({canvasLeft, y + 5.0f}, {fullMax.x, rowBottom - 5.0f}, cameraColor);
            float const centerY = (y + rowBottom) * 0.5f;
            for (auto const& [keyTick, _] : camera.keysByTick) {
                bool const dragging  = mDraggingKeyframeCameraId == camera.id && mDraggingKeyframeStartTick == keyTick;
                int const  drawnTick = dragging ? mDraggingKeyframeTick : keyTick;
                float      x         = scale.xAt(static_cast<float>(drawnTick));
                bool const selected =
                    selectedKeyframe && selectedKeyframe->trackId == camera.id && selectedKeyframe->tick == keyTick;
                ImVec2 const top{x, centerY - 5.0f};
                ImVec2 const right{x + 5.0f, centerY};
                ImVec2 const bottom{x, centerY + 5.0f};
                ImVec2 const left{x - 5.0f, centerY};
                ImU32 const  keyColor = !camera.enabled
                                          ? IM_COL32(132, 132, 132, 180)
                                          : (selected ? IM_COL32(244, 202, 47, 255) : theme::kIconHighlight);
                drawList->AddQuadFilled(top, right, bottom, left, keyColor);
                drawList->AddQuad(top, right, bottom, left, IM_COL32(24, 24, 24, 255), 1.0f);
                if (allowInput && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                    && std::abs(ImGui::GetMousePos().x - x) <= 7.0f && ImGui::GetMousePos().y >= y
                    && ImGui::GetMousePos().y <= rowBottom) {
                    clickConsumed = true;
                    ctx.selection.select(state::editing::model::SelectedKeyframe{camera.id, keyTick});
                    EditorAction previewAction{EditorActionType::SetPreviewCamera};
                    previewAction.id = camera.id;
                    ctx.submitAction(std::move(previewAction));
                    if (camera.locked || !camera.enabled) {
                        submitSeek(ctx, keyTick);
                    } else {
                        mDraggingKeyframeCameraId    = camera.id;
                        mDraggingKeyframeStartMouseX = ImGui::GetMousePos().x;
                        mDraggingKeyframeStartTick   = keyTick;
                        mDraggingKeyframeTick        = keyTick;
                        mDraggingKeyframeMoved       = false;
                    }
                }
            }
            // Runs after the keyframe pass so a keyframe hit always wins over the row.
            if (allowInput && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                && contains({canvasLeft, y + 5.0f}, {fullMax.x, rowBottom - 5.0f}, ImGui::GetMousePos())) {
                clickConsumed = true;
                ctx.selection.select(state::editing::model::SelectedCamera{camera.id});
                EditorAction action{EditorActionType::SetPreviewCamera};
                action.id = camera.id;
                ctx.submitAction(std::move(action));
            }
        }
        y = rowBottom + 2.0f;
    }

    if (allowInput && !mDraggingKeyframeCameraId.empty()) {
        if (std::abs(ImGui::GetMousePos().x - mDraggingKeyframeStartMouseX) > 2.0f) {
            mDraggingKeyframeMoved = true;
        }
        int const candidateTick =
            mDraggingKeyframeMoved ? boundedKeyframeTick(snapTick(tickFromMouse())) : mDraggingKeyframeStartTick;
        mDraggingKeyframeTick = candidateTick;
        float const markerX   = scale.xAt(static_cast<float>(candidateTick));
        drawList->AddLine({markerX, bodyTop}, {markerX, bodyBottom}, IM_COL32(240, 192, 32, 180), 2.0f);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (mDraggingKeyframeMoved) {
                if (candidateTick != mDraggingKeyframeStartTick) {
                    EditorAction action{EditorActionType::MoveCameraKeyframe};
                    action.id            = mDraggingKeyframeCameraId;
                    action.tick          = mDraggingKeyframeStartTick;
                    action.secondaryTick = candidateTick;
                    ctx.submitAction(std::move(action));
                }
            } else {
                submitSeek(ctx, mDraggingKeyframeStartTick);
            }
            mDraggingKeyframeCameraId.clear();
            mDraggingKeyframeStartTick = -1;
            mDraggingKeyframeMoved     = false;
        }
    }

    float const playheadX = std::clamp(scale.xAt(static_cast<float>(displayTick)), canvasLeft, fullMax.x);
    if (allowInput && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && std::abs(ImGui::GetMousePos().x - playheadX) <= 6.0f && ImGui::GetMousePos().y >= workTop
        && ImGui::GetMousePos().y < workBottom) {
        mDraggingPlayhead = true;
        clickConsumed     = true;
    }
    if (allowInput && mDraggingPlayhead) {
        displayTick = tickFromMouse();
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            submitSeek(ctx, displayTick);
            mDraggingPlayhead = false;
        }
    }
    if (allowInput && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && ImGui::GetMousePos().x >= canvasLeft && ImGui::GetMousePos().x <= fullMax.x
        && ImGui::GetMousePos().y >= bodyTop
        && ImGui::GetMousePos().y < workBottom - (maxScroll > 0.0f ? 18.0f : 0.0f)) {
        submitSeek(ctx, tickFromMouse());
    }

    float const visiblePlayheadX =
        std::clamp(scale.xAt(static_cast<float>(displayTick)), canvasLeft + 5.0f, fullMax.x - 5.0f);
    drawList->AddLine({visiblePlayheadX, bodyTop - 2.0f}, {visiblePlayheadX, bodyBottom}, kPlayheadColor, 1.5f);
    drawList->AddTriangleFilled(
        {visiblePlayheadX - 5.0f, bodyTop - 11.0f},
        {visiblePlayheadX + 5.0f, bodyTop - 11.0f},
        {visiblePlayheadX, bodyTop - 2.0f},
        kPlayheadColor
    );
    drawList->PopClipRect();
    ImVec2 const wheelMouse = ImGui::GetMousePos();
    bool const   canvasHovered =
        contains({canvasLeft, workTop}, {fullMax.x, workBottom}, wheelMouse) && wheelMouse.y < workBottom;
    bool const trackListHovered =
        wheelMouse.x >= fullMin.x && wheelMouse.x < canvasLeft && wheelMouse.y >= bodyTop && wheelMouse.y < workBottom;
    if (allowInput && (canvasHovered || trackListHovered) && ImGui::GetIO().MouseWheel != 0.0f) {
        float const wheel = ImGui::GetIO().MouseWheel;
        if (ImGui::GetIO().KeyShift) {
            float const anchorX    = std::clamp(wheelMouse.x - canvasLeft, 0.0f, canvasWidth);
            float const anchorTick = scale.tickAtExact(canvasLeft + anchorX);
            mZoomScale =
                std::clamp(mZoomScale * (wheel > 0.0f ? kZoomStep : 1.0f / kZoomStep), kMinZoomScale, kMaxZoomScale);
            float const nextPixelsPerTick = fitPixelsPerTick * mZoomScale;
            float const nextMaxScroll     = std::max(0.0f, state.totalTicks * nextPixelsPerTick - canvasWidth);
            mScrollX                      = std::clamp(anchorTick * nextPixelsPerTick - anchorX, 0.0f, nextMaxScroll);
        } else if (canvasHovered && maxScroll > 0.0f) {
            mScrollX = std::clamp(mScrollX - wheel * 60.0f, 0.0f, maxScroll);
        } else {
            mScrollY = std::clamp(mScrollY - wheel * 45.0f, 0.0f, maxScrollY);
        }
    }

    float const scrollbarHorizontalPadding = std::min(8.0f, canvasWidth * 0.25f);
    float const scrollbarWidth             = std::max(1.0f, canvasWidth - scrollbarHorizontalPadding * 2.0f);
    if (maxScroll > 0.0f) {
        ImGui::SetCursorScreenPos({canvasLeft + scrollbarHorizontalPadding, workBottom + 2.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(54, 54, 54, 255));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(137, 137, 137, 255));
        ImGui::SetNextItemWidth(scrollbarWidth);
        ImGui::SliderFloat("##timeline-scroll", &mScrollX, 0.0f, maxScroll, "", ImGuiSliderFlags_NoInput);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
    }
    ImGui::SetCursorScreenPos({fullMin.x, workBottom + scrollbarLaneHeight});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild("##TimelineTransport", {available.x, transportHeight}, false, ImGuiWindowFlags_NoScrollbar);
    float const buttonSize = iconButtonSize();
    char        speedLabel[32]{};
    std::snprintf(speedLabel, sizeof(speedLabel), "%.2fx", state.playbackSpeed);
    float const transportContentWidth = ImGui::GetContentRegionAvail().x;
    float const transportRowY         = std::max(0.0f, (transportHeight - buttonSize) * 0.5f);

    std::string const timecode = formatTick(displayTick) + " / " + formatTick(state.totalTicks);
    ImGui::SetCursorPos({8.0f, transportRowY});
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", timecode.c_str());

    auto const startTooltip  = "playback.refactorEditor.timeline.skipToStart"_tr();
    auto const prevTooltip   = "playback.refactorEditor.timeline.previousTick"_tr();
    auto const playTooltip   = "playback.refactorEditor.timeline.play"_tr();
    auto const pauseTooltip  = "playback.refactorEditor.timeline.pause"_tr();
    auto const nextTooltip   = "playback.refactorEditor.timeline.nextTick"_tr();
    auto const endTooltip    = "playback.refactorEditor.timeline.skipToEnd"_tr();
    auto const slowerTooltip = "playback.refactorEditor.timeline.decreaseSpeed"_tr();
    auto const fasterTooltip = "playback.refactorEditor.timeline.increaseSpeed"_tr();

    float const playbackWidth = buttonSize * 5.0f + 4.0f;
    ImGui::SetCursorPos({std::max(0.0f, (transportContentWidth - playbackWidth) * 0.5f), transportRowY});
    if (iconButton("transport-start", ICON_SKIP_BACK, startTooltip.c_str())) seekTo(ctx, 0);
    sameIcon();
    if (iconButton("transport-prev", ICON_CHEVRONS_LEFT, prevTooltip.c_str())) seekRelative(ctx, -1);
    sameIcon();
    if (iconButton(
            "transport-play",
            state.paused ? ICON_PLAY : ICON_PAUSE,
            state.paused ? playTooltip.c_str() : pauseTooltip.c_str()
        )) {
        ctx.submitAction({EditorActionType::TogglePause});
    }
    sameIcon();
    if (iconButton("transport-next", ICON_CHEVRONS_RIGHT, nextTooltip.c_str())) seekRelative(ctx, 1);
    sameIcon();
    if (iconButton("transport-end", ICON_SKIP_FORWARD, endTooltip.c_str())) seekTo(ctx, state.totalTicks);

    float const speedWidth = buttonSize * 2.0f + 4.0f + ImGui::CalcTextSize(speedLabel).x;
    ImGui::SetCursorPos({std::max(0.0f, transportContentWidth - speedWidth - 8.0f), transportRowY});
    if (iconButton("speed-down", "-", slowerTooltip.c_str())) ctx.submitAction({EditorActionType::DecreaseSpeed});
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", speedLabel);
    ImGui::SameLine(0.0f, 2.0f);
    if (iconButton("speed-up", "+", fasterTooltip.c_str())) ctx.submitAction({EditorActionType::IncreaseSpeed});
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

} // namespace playback::editor::ui
