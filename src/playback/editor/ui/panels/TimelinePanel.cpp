#include "TimelinePanel.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/TimelineScale.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/replay/ReplaySession.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <ranges>

namespace playback::editor::ui {
using namespace playback::state;

using namespace ll::i18n_literals;

namespace {

constexpr float kMinZoomScale   = 1.0f;
constexpr float kMaxZoomScale   = 20.0f;
constexpr float kZoomStep       = 1.15f;
constexpr int   kTicksPerSecond = widgets::kTicksPerSecond;
constexpr int   kSnapTickRadius = 6;

using widgets::formatTick;
using widgets::formatTickCompact;
using widgets::iconButton;
using widgets::iconButtonSize;
using widgets::iconToggle;

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

float maxScrollFor(float canvasWidth, float zoom, int totalTicks) {
    float const fit = canvasWidth / static_cast<float>(std::max(1, totalTicks));
    return std::max(0.0f, static_cast<float>(totalTicks) * fit * zoom - canvasWidth);
}

void drawKeyframeDiamond(ImDrawList* drawList, float x, float centerY, float radius, ImU32 fill, ImU32 outline) {
    ImVec2 const top{x, centerY - radius};
    ImVec2 const right{x + radius, centerY};
    ImVec2 const bottom{x, centerY + radius};
    ImVec2 const left{x - radius, centerY};
    drawList->AddQuadFilled(top, right, bottom, left, fill);
    drawList->AddQuad(top, right, bottom, left, outline, 1.0f);
}

void drawKeyframeSquare(ImDrawList* drawList, float x, float centerY, float radius, ImU32 fill, ImU32 outline) {
    float const h = radius * 0.8f;
    drawList->AddRectFilled({x - h, centerY - h}, {x + h, centerY + h}, fill);
    drawList->AddRect({x - h, centerY - h}, {x + h, centerY + h}, outline, 0.0f, 0, 1.0f);
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
    mRangeDragMode             = 0;
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
    std::string cameraId = resolveKeyframeTargetCamera(ctx);
    if (cameraId.empty()) return false;

    EditorAction action{EditorActionType::AddCameraKeyframe};
    action.id   = cameraId;
    action.tick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    ctx.submitAction(std::move(action));
    ctx.selection.select(state::editing::model::SelectedCamera{std::move(cameraId)});
    return true;
}

// Mirrors Flashback's find-or-create camera track so the shortcut works without a prior selection.
std::string TimelinePanel::resolveKeyframeTargetCamera(PanelContext const& ctx) const {
    auto const project = ctx.state.project;
    if (!project) return {};

    auto const isUsable = [&](std::string const& id) {
        return std::ranges::any_of(project->cameras, [&](auto const& camera) {
            return camera.id == id && !camera.locked;
        });
    };

    if (auto const* selected = ctx.selection.getAs<state::editing::model::SelectedCamera>();
        selected && isUsable(selected->cameraId)) {
        return selected->cameraId;
    }
    if (auto const* selected = ctx.selection.getAs<state::editing::model::SelectedKeyframe>();
        selected && isUsable(selected->trackId)) {
        return selected->trackId;
    }
    for (auto const& camera : project->cameras) {
        if (camera.enabled && !camera.locked) return camera.id;
    }
    for (auto const& camera : project->cameras) {
        if (!camera.locked) return camera.id;
    }
    return {};
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

void TimelinePanel::drawTitleLabel(PanelContext const& ctx, float height) {
    auto const        project = ctx.state.project;
    std::string const title   = "playback.refactorEditor.timeline.title"_tr();
    float const       rowY    = std::max(0.0f, widgets::textOffsetInBox(height, title.c_str()));
    // ImGui adds no offset of its own here, so the cursor Y is the text's draw Y.
    ImGui::SetCursorPos({metrics::gutter(), rowY});
    ImGui::TextDisabled("%s", title.c_str());
    if (!project) return;
    ImGui::SameLine(0.0f, metrics::gutter());
    ImGui::TextDisabled("%s", "playback.refactorEditor.timeline.trackCount"_tr(project->cameras.size()).c_str());
}

void TimelinePanel::drawTransportGroup(PanelContext const& ctx, int displayTick, float width) {
    auto const&       state    = ctx.state;
    float const       unit     = metrics::iconButton();
    float const       rowY     = std::max(0.0f, (metrics::toolbarRow() - unit) * 0.5f);
    std::string const timecode = formatTick(displayTick) + "  /  " + formatTick(state.totalTicks);
    char              speedLabel[24]{};
    std::snprintf(speedLabel, sizeof(speedLabel), "%.2fx", state.playbackSpeed);

    float const timeWidth  = ImGui::CalcTextSize(timecode.c_str()).x;
    float const speedWidth = widgets::dropdownChipWidth(speedLabel);
    float const groupWidth = timeWidth + metrics::gutter() * 2.0f + unit * 5.0f + 4.0f + metrics::gutter() * 2.0f
                           + speedWidth + metrics::gutter() * 2.0f + unit;
    float const startX     = std::max(0.0f, (width - groupWidth) * 0.5f);

    // SameLine() restores Y to the line's first item, so the timecode is drawn rather than submitted.
    ImGui::SetCursorPos({startX, rowY});
    ImVec2 const buttonOrigin = ImGui::GetCursorScreenPos();
    ImVec2 const timePos{buttonOrigin.x, widgets::textYForCentre(buttonOrigin.y + unit * 0.5f, timecode.c_str())};
    ImGui::GetWindowDrawList()->AddText(timePos, theme::kTextDim, timecode.c_str());

    ImGui::SetCursorPos({startX + timeWidth + metrics::gutter() * 2.0f, rowY});
    using widgets::VectorIcon;
    if (widgets::vectorIconButton(
            "transport-start",
            VectorIcon::SkipStart,
            "playback.refactorEditor.timeline.skipToStart"_tr().c_str()
        ))
        seekTo(ctx, 0);
    ImGui::SameLine(0.0f, 1.0f);
    if (widgets::vectorIconButton(
            "transport-prev",
            VectorIcon::StepBack,
            "playback.refactorEditor.timeline.previousTick"_tr().c_str()
        ))
        seekRelative(ctx, -1);
    ImGui::SameLine(0.0f, 1.0f);
    if (widgets::vectorIconButton(
            "transport-play",
            state.paused ? VectorIcon::Play : VectorIcon::Pause,
            (state.paused ? "playback.refactorEditor.timeline.play"_tr()
                          : "playback.refactorEditor.timeline.pause"_tr())
                .c_str()
        )) {
        ctx.submitAction({EditorActionType::TogglePause});
    }
    ImGui::SameLine(0.0f, 1.0f);
    if (widgets::vectorIconButton(
            "transport-next",
            VectorIcon::StepForward,
            "playback.refactorEditor.timeline.nextTick"_tr().c_str()
        ))
        seekRelative(ctx, 1);
    ImGui::SameLine(0.0f, 1.0f);
    if (widgets::vectorIconButton(
            "transport-end",
            VectorIcon::SkipEnd,
            "playback.refactorEditor.timeline.skipToEnd"_tr().c_str()
        ))
        seekTo(ctx, state.totalTicks);

    ImGui::SameLine(0.0f, metrics::gutter() * 2.0f);
    if (widgets::dropdownChip(
            "transport-speed",
            speedLabel,
            "playback.refactorEditor.timeline.speedTooltip"_tr().c_str()
        )) {
        ImGui::OpenPopup("##transport-speed-menu");
    }
    // The menu lists the speeds themselves; stepping up and down stays on the keyboard shortcuts.
    if (ImGui::BeginPopup("##transport-speed-menu")) {
        for (float const speed : replay::ReplaySession::playbackSpeeds()) {
            char label[16]{};
            std::snprintf(label, sizeof(label), "%gx", static_cast<double>(speed));
            if (ImGui::MenuItem(label, nullptr, std::abs(state.playbackSpeed - speed) < 0.0001f)) {
                EditorAction action{EditorActionType::SetPlaybackSpeed};
                action.speed = speed;
                ctx.submitAction(std::move(action));
            }
        }
        ImGui::EndPopup();
    }

    bool const maximized = ctx.commands.isViewportMaximized();
    ImGui::SameLine(0.0f, metrics::gutter() * 2.0f);
    if (widgets::vectorIconButton(
            "transport-maximize",
            maximized ? VectorIcon::Restore : VectorIcon::Maximize,
            (maximized ? "playback.refactorEditor.timeline.restore"_tr()
                       : "playback.refactorEditor.timeline.maximize"_tr())
                .c_str(),
            maximized
        )) {
        ctx.commands.toggleViewportMaximized();
    }
}

void TimelinePanel::drawTitleRow(PanelContext const& ctx, Layout const& layout, int displayTick, bool allowInput) {
    float const height   = metrics::toolbarRow();
    float const width    = layout.fullMax.x - layout.fullMin.x;
    auto*       drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(layout.fullMin, {layout.fullMax.x, layout.titleBottom}, theme::kBgHeader);
    drawList->AddLine({layout.fullMin.x, layout.titleBottom}, {layout.fullMax.x, layout.titleBottom}, theme::kBorder);

    ImGui::SetCursorScreenPos(layout.fullMin);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild("##TimelineTitle", {width, height}, false, ImGuiWindowFlags_NoScrollbar);
    drawTitleLabel(ctx, height);
    drawTransportGroup(ctx, displayTick, width);

    // Zooming lives on the range bar handles now; only the reset-to-fit affordance stays here.
    float const unit = metrics::iconButton();
    ImGui::SetCursorPos({std::max(0.0f, width - unit - metrics::gutter()), std::max(0.0f, (height - unit) * 0.5f)});
    if (iconButton("timeline-zoom-reset", ICON_RESET, "playback.refactorEditor.timeline.zoomReset"_tr().c_str()))
        resetZoom();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    (void)allowInput;
}

void TimelinePanel::drawTrackHeaders(PanelContext const& ctx, Layout const& layout, bool allowInput) {
    auto const  project  = ctx.state.project;
    auto*       drawList = ImGui::GetWindowDrawList();
    float const listLeft = layout.fullMin.x;
    float const listW    = layout.listWidth;

    drawList
        ->AddRectFilled({listLeft, layout.rulerTop}, {listLeft + listW, layout.bodyBottom}, theme::kTimelineSidebar);

    ImGui::SetCursorScreenPos({listLeft, layout.rulerTop});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild(
        "##TimelineTrackControls",
        {listW, layout.bodyTop - layout.rulerTop},
        false,
        ImGuiWindowFlags_NoScrollbar
    );
    float const controlSize = metrics::iconButton();
    ImGui::SetCursorPos({metrics::gutter() * 0.5f, std::max(0.0f, (layout.headerHeight - controlSize) * 0.5f)});
    if (iconButton("timeline-add", ICON_ADD, "playback.refactorEditor.timeline.addTrack"_tr().c_str()))
        ImGui::OpenPopup("##timeline-add-menu");
    if (ImGui::BeginPopup("##timeline-add-menu")) {
        // An empty name lets the ops layer pick the next free localized default.
        if (ImGui::MenuItem("playback.refactorEditor.details.addFreeCamera"_tr().c_str())) {
            ctx.submitAction({EditorActionType::AddFreeCamera});
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, 1.0f);
    if (iconToggle(
            "timeline-snap",
            ICON_TRACK_ACTIVE,
            "playback.refactorEditor.timeline.snapTooltip"_tr().c_str(),
            mSnapEnabled
        )) {
        mSnapEnabled = !mSnapEnabled;
    }
    ImGui::SameLine(0.0f, metrics::gutter() * 0.5f);
    // The input is frame-height, not button-height; centre it on the same line as the buttons.
    ImGui::SetCursorPosY(std::max(0.0f, (layout.headerHeight - ImGui::GetFrameHeight()) * 0.5f));
    char search[128]{};
    std::snprintf(search, sizeof(search), "%s", mTrackSearch.c_str());
    ImGui::SetNextItemWidth(std::max(32.0f, ImGui::GetContentRegionAvail().x - metrics::gutter() * 0.5f));
    if (ImGui::InputTextWithHint(
            "##timeline-search",
            "playback.refactorEditor.timeline.searchTracks"_tr().c_str(),
            search,
            sizeof(search)
        )) {
        mTrackSearch = search;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SetCursorScreenPos({listLeft, layout.bodyTop});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild(
        "##TimelineTrackList",
        {listW, layout.bodyBottom - layout.bodyTop},
        false,
        ImGuiWindowFlags_NoScrollbar
    );
    drawList->PushClipRect({listLeft, layout.bodyTop}, {listLeft + listW, layout.bodyBottom}, true);

    auto const* selectedCamera   = ctx.selection.getAs<state::editing::model::SelectedCamera>();
    auto const* selectedKeyframe = ctx.selection.getAs<state::editing::model::SelectedKeyframe>();
    float       rowY             = layout.bodyTop - mScrollY;
    for (auto const& row : mTrackTree.rows()) {
        float const rowBottom = rowY + row.height;
        if (rowBottom < layout.bodyTop || rowY > layout.bodyBottom) {
            rowY = rowBottom;
            continue;
        }
        bool const selected = !row.cameraId.empty()
                           && ((selectedCamera && selectedCamera->cameraId == row.cameraId)
                               || (selectedKeyframe && selectedKeyframe->trackId == row.cameraId));

        ImGui::SetCursorScreenPos({listLeft, rowY});
        ImGui::InvisibleButton(("##track-row-" + row.id).c_str(), {listW, row.height});
        bool const  hovered        = ImGui::IsItemHovered();
        bool const  clicked        = allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Left);
        float const selectedAmount = mAnimator.animate("row-selected", row.id, selected ? 1.0f : 0.0f);
        float const hoverAmount    = mAnimator.animate("row-hover", row.id, hovered ? 1.0f : 0.0f);
        float const highlight      = std::max(selectedAmount, hoverAmount);
        if (highlight > 0.0f) {
            drawList->AddRectFilled(
                {listLeft, rowY},
                {listLeft + listW, rowBottom},
                lerpColor(theme::kTimelineSidebar, theme::kRowHighlight, highlight)
            );
        }
        if (selectedAmount > 0.0f) {
            drawList->AddRect(
                {listLeft + 1.0f, rowY},
                {listLeft + listW - 1.0f, rowBottom},
                lerpColor(theme::withAlpha(theme::kRowSelectedBorder, 0), theme::kRowSelectedBorder, selectedAmount),
                0.0f,
                0,
                1.0f
            );
        }
        ImU32 const swatch = row.enabled ? theme::trackColor(row.cameraIndex) : theme::kTrackDisabled;
        drawList->AddRectFilled(
            {listLeft, rowY},
            {listLeft + metrics::trackSwatch(), rowBottom},
            lerpColor(theme::withAlpha(swatch, 0x99), swatch, selectedAmount)
        );

        float const indent = metrics::gutter() + metrics::trackSwatch();
        float       labelX = listLeft + indent;

        ImU32 const textColor = row.enabled ? theme::kText : theme::kTextDim;
        std::string label     = row.name;
        // Text centres on the fractional row, then the icon centres on the text's optical middle.
        float const rowCentre = (rowY + rowBottom) * 0.5f;
        float const textY     = widgets::textYForCentre(rowCentre, label.c_str());
        float const iconBox   = metrics::iconGlyph();
        widgets::drawIconAtCentre(
            drawList,
            ICON_CAMERA,
            {labelX + iconBox * 0.5f, widgets::textInkCentre(textY, label.c_str())},
            textColor
        );
        labelX += iconBox + metrics::gutter() * 0.5f;
        drawList->AddText({labelX, textY}, textColor, label.c_str());

        if (row.kind == TrackRowKind::Camera) {
            float const  controlWidth = metrics::iconGlyph();
            float const  eyeX         = listLeft + listW - controlWidth - metrics::gutter();
            ImVec2 const mouse        = ImGui::GetMousePos();
            bool const   eyeHovered   = contains({eyeX, rowY}, {eyeX + controlWidth, rowBottom}, mouse);
            if (eyeHovered) {
                widgets::hoverTooltip((row.enabled ? "playback.refactorEditor.timeline.disableTrack"_tr()
                                                   : "playback.refactorEditor.timeline.enableTrack"_tr())
                                          .c_str());
            }
            if (allowInput && eyeHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                EditorAction action{EditorActionType::SetCameraEnabled};
                action.id    = row.cameraId;
                action.value = !row.enabled;
                ctx.submitAction(std::move(action));
            }
            char const* eyeIcon = row.enabled ? ICON_EYE : ICON_HIDE;
            widgets::drawIconAtCentre(
                drawList,
                eyeIcon,
                {eyeX + controlWidth * 0.5f, rowCentre},
                eyeHovered ? theme::kIconHighlight : theme::kIconInactive
            );
            if (row.locked) {
                widgets::drawIconAtCentre(
                    drawList,
                    ICON_LOCK,
                    {eyeX - controlWidth * 0.5f, rowCentre},
                    theme::kTextDim
                );
            }
            if (clicked && mouse.x < eyeX) {
                ctx.selection.select(state::editing::model::SelectedCamera{row.cameraId});
                EditorAction action{EditorActionType::SetPreviewCamera};
                action.id = row.cameraId;
                ctx.submitAction(std::move(action));
            }
        }
        rowY = rowBottom;
    }
    drawList->PopClipRect();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    drawList->AddLine({listLeft + listW, layout.rulerTop}, {listLeft + listW, layout.fullMax.y}, theme::kLine);
}

void TimelinePanel::drawRuler(
    PanelContext const&  ctx,
    Layout const&        layout,
    TimelineScale const& scale,
    int                  displayTick
) {
    auto const& state    = ctx.state;
    auto*       drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        {layout.canvasLeft, layout.rulerTop},
        {layout.fullMax.x, layout.bodyTop},
        theme::kTimelineRuler
    );
    drawList->AddLine({layout.canvasLeft, layout.bodyTop}, {layout.fullMax.x, layout.bodyTop}, theme::kLine);

    std::string const longest       = formatTickCompact(state.totalTicks);
    float const       minimumMajor  = std::max(metrics::font() * 6.0f, ImGui::CalcTextSize(longest.c_str()).x + 30.0f);
    int const         majorStep     = majorTickStep(scale.pixelsPerTick, minimumMajor);
    int const         minorStep     = std::max(1, majorStep / 5);
    int const         firstTick     = scale.firstVisibleTick(minorStep);
    float const       rulerBaseline = layout.bodyTop - 1.0f;
    float             lastLabelEnd  = layout.canvasLeft - 6.0f;
    for (int tick = firstTick; tick <= state.totalTicks; tick += minorStep) {
        float const x = scale.xAt(static_cast<float>(tick));
        if (x < layout.canvasLeft || x > layout.fullMax.x) continue;
        bool const  major      = tick % majorStep == 0;
        float const tickHeight = major ? 8.0f : 4.0f;
        drawList->AddLine(
            {x, rulerBaseline - tickHeight},
            {x, rulerBaseline},
            major ? theme::kRulerMajor : theme::kRulerMinor
        );
        if (!major) continue;
        std::string const label      = formatTickCompact(tick);
        float const       labelWidth = ImGui::CalcTextSize(label.c_str()).x;
        float const       labelX     = x + 3.0f;
        float const       labelY     = layout.rulerTop + (layout.headerHeight - 8.0f - metrics::font()) * 0.5f;
        // Skip instead of clamping into place; clamping stacks labels on top of each other at the edges.
        if (labelX >= lastLabelEnd + 6.0f && labelX + labelWidth <= layout.fullMax.x - 4.0f) {
            drawList->AddText({labelX, labelY}, theme::kTextDim, label.c_str());
            lastLabelEnd = labelX + labelWidth;
        }
    }
    (void)displayTick;
}

void TimelinePanel::drawTrackGrid(PanelContext const& ctx, Layout const& layout, TimelineScale const& scale) {
    auto const& state    = ctx.state;
    auto*       drawList = ImGui::GetWindowDrawList();

    std::string const longest      = formatTickCompact(state.totalTicks);
    float const       minimumMajor = std::max(metrics::font() * 6.0f, ImGui::CalcTextSize(longest.c_str()).x + 30.0f);
    int const         majorStep    = majorTickStep(scale.pixelsPerTick, minimumMajor);
    for (int tick = scale.firstVisibleTick(majorStep); tick <= state.totalTicks; tick += majorStep) {
        float const x = scale.xAt(static_cast<float>(tick));
        if (x < layout.canvasLeft || x > layout.fullMax.x) continue;
        drawList->AddLine({x, layout.bodyTop}, {x, layout.bodyBottom}, theme::kTrackGrid);
    }
}

void TimelinePanel::drawExportRange(
    PanelContext const&  ctx,
    Layout const&        layout,
    TimelineScale const& scale,
    bool                 allowInput
) {
    int const total = ctx.state.totalTicks;
    if (total <= 0) return;
    // Unset means "whole replay", so nothing is drawn until the user marks a point (Flashback's rule).
    int const inTick  = ctx.commands.exportStartTick();
    int const outTick = ctx.commands.exportEndTick();
    if (inTick < 0 || outTick < 0) return;

    auto*       drawList = ImGui::GetWindowDrawList();
    float const left     = scale.xAt(static_cast<float>(std::clamp(inTick, 0, total)));
    float const right    = scale.xAt(static_cast<float>(std::clamp(outTick, 0, total)));
    float const top      = layout.rulerTop;
    float const bottom   = layout.bodyTop;
    drawList->PushClipRect({layout.canvasLeft, top}, {layout.fullMax.x, bottom}, true);

    // The band lives in the ruler strip, bracketed at each end; the bracket is the grab handle.
    drawList->AddRectFilled({left, top}, {right, bottom}, theme::kExportBand);
    ImVec2 const mouse  = ImGui::GetMousePos();
    bool const   inBand = allowInput && mouse.y >= top && mouse.y <= bottom;
    float const  stem   = std::max(1.0f, 1.5f * metrics::scale());
    float const  jaw    = std::max(4.0f, metrics::gutter() * 0.9f);
    auto const   marker = [&](float x, bool isIn, int dragMode) {
        // Grab area stays wide even though the bracket is thin.
        bool const  hovered = inBand && std::abs(mouse.x - x) <= jaw + 3.0f;
        ImU32 const color = (mExportMarkerDrag == dragMode || hovered) ? theme::kIconHighlight : theme::kExportBracket;
        float const inner = isIn ? x + stem : x - stem;
        float const tip   = isIn ? x + jaw : x - jaw;
        drawList->AddRectFilled({std::min(x, inner), top}, {std::max(x, inner), bottom}, color);
        // Jaws turn the stem into a bracket that faces into the range.
        drawList->AddRectFilled({std::min(inner, tip), top}, {std::max(inner, tip), top + stem}, color);
        drawList->AddRectFilled({std::min(inner, tip), bottom - stem}, {std::max(inner, tip), bottom}, color);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) mExportMarkerDrag = dragMode;
        return hovered;
    };
    bool const overIn  = marker(left, true, 1);
    bool const overOut = marker(right, false, 2);
    if (overIn || overOut || mExportMarkerDrag != 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    drawList->PopClipRect();

    if (mExportMarkerDrag == 0) return;
    if (!allowInput || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        mExportMarkerDrag = 0;
        return;
    }
    // Shift snaps to the nearest keyframe, matching the modifier used elsewhere on the timeline.
    int dragged = std::clamp(scale.tickAt(mouse.x), 0, total);
    if (ImGui::GetIO().KeyShift && ctx.project()) {
        int best     = dragged;
        int bestDist = std::numeric_limits<int>::max();
        for (auto const& camera : ctx.project()->cameras) {
            for (auto const& [tick, key] : camera.keysByTick) {
                int const distance = std::abs(tick - dragged);
                if (distance < bestDist) {
                    bestDist = distance;
                    best     = tick;
                }
            }
        }
        if (bestDist <= kSnapTickRadius) dragged = best;
    }
    if (mExportMarkerDrag == 1) ctx.commands.setExportStartTick(std::min(dragged, outTick));
    else ctx.commands.setExportEndTick(std::max(dragged, inTick));
}

void TimelinePanel::drawRangeBar(PanelContext const& ctx, Layout const& layout, bool allowInput) {
    auto const& state     = ctx.state;
    auto*       drawList  = ImGui::GetWindowDrawList();
    float const barTop    = layout.bodyBottom + 1.0f;
    float const barLeft   = layout.canvasLeft;
    float const barWidth  = layout.canvasWidth;
    float const barHeight = layout.fullMax.y - barTop;
    if (barWidth <= 8.0f || barHeight <= 4.0f) return;

    drawList->AddRectFilled({layout.fullMin.x, barTop}, {layout.fullMax.x, layout.fullMax.y}, theme::kBgHeader);

    // The bar is the visible window over the whole replay: round grips scale, the body pans.
    float const windowSpan  = std::clamp(1.0f / std::max(kMinZoomScale, mZoomScale), 0.02f, 1.0f);
    float const maxScroll   = maxScrollFor(layout.canvasWidth, mZoomScale, state.totalTicks);
    float const windowStart = maxScroll > 0.0f ? (mScrollX / maxScroll) * (1.0f - windowSpan) : 0.0f;
    float const centreY     = (barTop + layout.fullMax.y) * 0.5f;
    float const grip        = std::min(barHeight, metrics::rangeBar()) * 0.5f - 1.0f;
    float const trackH      = std::max(3.0f, grip * 0.7f);
    // Grips are centred on the window edges, so the track must inset by their radius to stay inside.
    float const railLeft  = barLeft + grip;
    float const railWidth = std::max(1.0f, barWidth - grip * 2.0f);
    float const winLeft   = railLeft + windowStart * railWidth;
    float const winRight  = winLeft + windowSpan * railWidth;

    drawList->AddRectFilled(
        {railLeft, centreY - trackH * 0.5f},
        {railLeft + railWidth, centreY + trackH * 0.5f},
        theme::kInputBg,
        trackH * 0.5f
    );

    ImGui::SetCursorScreenPos({barLeft, barTop});
    ImGui::InvisibleButton("##timeline-range", {barWidth, barHeight});
    ImVec2 const mouse       = ImGui::GetMousePos();
    float const  hitRadius   = grip + 3.0f;
    bool const   barHovered  = ImGui::IsItemHovered();
    bool const   nearLeft    = barHovered && std::abs(mouse.x - winLeft) <= hitRadius;
    bool const   nearRight   = barHovered && std::abs(mouse.x - winRight) <= hitRadius;
    bool const   leftActive  = mRangeDragMode == 1 || (mRangeDragMode == 0 && nearLeft);
    bool const   rightActive = mRangeDragMode == 2 || (mRangeDragMode == 0 && nearRight && !nearLeft);
    bool const   bodyActive  = mRangeDragMode == 3 || (mRangeDragMode == 0 && barHovered && !nearLeft && !nearRight);

    drawList->AddRectFilled(
        {winLeft, centreY - trackH * 0.5f},
        {winRight, centreY + trackH * 0.5f},
        bodyActive ? theme::kAccentHover : theme::kAccent,
        trackH * 0.5f
    );
    // A dark outline around the whole rail contains the accent fill instead of letting it float.
    drawList->AddRect(
        {railLeft, centreY - trackH * 0.5f},
        {railLeft + railWidth, centreY + trackH * 0.5f},
        theme::kGripRing,
        trackH * 0.5f,
        0,
        1.0f
    );

    auto const gripAt = [&](float x, bool active) {
        drawList->AddCircleFilled({x, centreY}, grip, active ? theme::kIconHighlight : theme::kGripBody);
        drawList->AddCircle({x, centreY}, grip, theme::kGripRing, 0, 1.0f);
    };
    gripAt(winLeft, leftActive);
    gripAt(winRight, rightActive);

    if (!allowInput) {
        mRangeDragMode = 0;
        return;
    }
    if (barHovered) ImGui::SetMouseCursor(nearLeft || nearRight ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_Hand);
    if (ImGui::IsItemActivated()) {
        if (nearLeft) mRangeDragMode = 1;
        else if (nearRight) mRangeDragMode = 2;
        else if (mouse.x > winLeft && mouse.x < winRight) mRangeDragMode = 3;
        else mRangeDragMode = 4;
        mRangeDragOriginX      = mouse.x;
        mRangeDragOriginScroll = mScrollX;
        mRangeDragOriginZoom   = mZoomScale;
    }
    if (ImGui::IsItemDeactivated()) mRangeDragMode = 0;
    if (!ImGui::IsItemActive() || mRangeDragMode == 0) {
        if (ImGui::IsItemDeactivated() && mRangeDragMode == 4) resetZoom();
        return;
    }

    float const spanBefore      = std::clamp(1.0f / std::max(kMinZoomScale, mRangeDragOriginZoom), 0.02f, 1.0f);
    float const maxScrollBefore = maxScrollFor(layout.canvasWidth, mRangeDragOriginZoom, state.totalTicks);
    float const startBefore =
        maxScrollBefore > 0.0f ? (mRangeDragOriginScroll / maxScrollBefore) * (1.0f - spanBefore) : 0.0f;
    float const delta = (mouse.x - mRangeDragOriginX) / railWidth;

    float newStart = startBefore;
    float newSpan  = spanBefore;
    if (mRangeDragMode == 1) {
        newStart = std::clamp(startBefore + delta, 0.0f, startBefore + spanBefore - 0.02f);
        newSpan  = startBefore + spanBefore - newStart;
    } else if (mRangeDragMode == 2) {
        newSpan = std::clamp(spanBefore + delta, 0.02f, 1.0f - startBefore);
    } else if (mRangeDragMode == 3) {
        newStart = std::clamp(startBefore + delta, 0.0f, 1.0f - spanBefore);
    } else {
        return;
    }

    mZoomScale               = std::clamp(1.0f / std::max(0.02f, newSpan), kMinZoomScale, kMaxZoomScale);
    float const clampedSpan  = std::clamp(1.0f / mZoomScale, 0.02f, 1.0f);
    float const maxScrollNow = maxScrollFor(layout.canvasWidth, mZoomScale, state.totalTicks);
    float const startSpan    = std::max(0.0001f, 1.0f - clampedSpan);
    mScrollX                 = std::clamp(newStart / startSpan * maxScrollNow, 0.0f, maxScrollNow);
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
        mRangeDragMode             = 0;
    }

    mAnimator.beginFrame();
    mTrackTree.setSearch(mTrackSearch);
    mTrackTree.rebuild(*project);
    int displayTick = mPendingSeekTick >= 0 ? mPendingSeekTick : state.currentTick;
    if (mRulerDragTick >= 0) displayTick = mRulerDragTick;
    if (mPendingSeekTick >= 0 && state.currentTick == mPendingSeekTick) mPendingSeekTick = -1;

    ImVec2 const fullMin   = ImGui::GetCursorScreenPos();
    ImVec2 const available = ImGui::GetContentRegionAvail();
    if (available.x < 220.0f || available.y < 120.0f) return;

    Layout layout;
    layout.fullMin     = fullMin;
    layout.fullMax     = {fullMin.x + available.x, fullMin.y + available.y};
    layout.titleBottom = fullMin.y + metrics::toolbarRow();
    layout.rulerTop    = layout.titleBottom;
    // The band beside the ruler holds the add button and search field, so it follows the control height.
    layout.headerHeight = std::max(metrics::ruler(), metrics::iconButton() + 4.0f);
    layout.bodyTop      = layout.rulerTop + layout.headerHeight;
    layout.bodyBottom   = layout.fullMax.y - metrics::rangeBar();

    float const fontSize         = ImGui::GetFontSize();
    float const minimumListWidth = std::max(fontSize * 10.0f, 140.0f);
    layout.listWidth             = std::clamp(
        available.x * mTrackListWidthRatio,
        minimumListWidth,
        available.x - std::max(fontSize * 12.0f, 180.0f)
    );
    layout.canvasLeft  = fullMin.x + layout.listWidth + metrics::splitter();
    layout.canvasWidth = layout.fullMax.x - layout.canvasLeft;

    auto* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(fullMin, layout.fullMax, theme::kTimelineBg);

    drawTitleRow(ctx, layout, displayTick, allowInput);

    float const fitPixelsPerTick = layout.canvasWidth / static_cast<float>(std::max(1, state.totalTicks));
    float const pixelsPerTick    = fitPixelsPerTick * mZoomScale;
    float const maxScroll        = std::max(0.0f, state.totalTicks * pixelsPerTick - layout.canvasWidth);
    mScrollX                     = std::clamp(mScrollX, 0.0f, maxScroll);

    TimelineScale const scale{layout.canvasLeft, layout.canvasWidth, pixelsPerTick, mScrollX, state.totalTicks};

    float trackContentHeight = 0.0f;
    for (auto const& row : mTrackTree.rows()) trackContentHeight += row.height;
    float const visibleTrackHeight = std::max(0.0f, layout.bodyBottom - layout.bodyTop);
    float const maxScrollY         = std::max(0.0f, trackContentHeight - visibleTrackHeight);
    mScrollY                       = std::clamp(mScrollY, 0.0f, maxScrollY);

    ImGui::SetCursorScreenPos({fullMin.x + layout.listWidth - metrics::splitter() * 0.5f, layout.rulerTop});
    ImGui::InvisibleButton("##timeline-list-splitter", {metrics::splitter(), layout.bodyBottom - layout.rulerTop});
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (allowInput && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        mTrackListWidthRatio = std::clamp((ImGui::GetMousePos().x - fullMin.x) / available.x, 0.18f, 0.55f);
    }

    drawTrackHeaders(ctx, layout, allowInput);

    drawList
        ->AddRectFilled({layout.canvasLeft, layout.bodyTop}, {layout.fullMax.x, layout.bodyBottom}, theme::kTimelineBg);
    drawList->PushClipRect({layout.canvasLeft, layout.rulerTop}, {layout.fullMax.x, layout.bodyBottom}, true);
    drawRuler(ctx, layout, scale, displayTick);

    ImGui::SetCursorScreenPos({layout.canvasLeft, layout.rulerTop});
    ImGui::InvisibleButton("##timeline-ruler", {layout.canvasWidth, layout.bodyTop - layout.rulerTop});
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
    float const keyRadius        = metrics::keyframeRadius();
    float       y                = layout.bodyTop - mScrollY;
    bool        clickConsumed    = false;
    int         bandIndex        = 0;
    // Alternating bands continue past the last track so the empty area still reads as timeline lanes.
    {
        float const bandHeight = metrics::cameraRow();
        int         band       = 0;
        for (float bandY = layout.bodyTop - mScrollY; bandY < layout.bodyBottom; bandY += bandHeight, ++band) {
            if (bandY + bandHeight < layout.bodyTop) continue;
            if (band % 2 == 0) continue;
            drawList->AddRectFilled(
                {layout.canvasLeft, std::max(bandY, layout.bodyTop)},
                {layout.fullMax.x, std::min(bandY + bandHeight, layout.bodyBottom)},
                theme::kTimelineTrackBg
            );
        }
    }
    for (auto const& row : mTrackTree.rows()) {
        float const rowBottom = y + row.height;
        if (rowBottom < layout.bodyTop || y > layout.bodyBottom) {
            y = rowBottom;
            ++bandIndex;
            continue;
        }
        float const centerY = (y + rowBottom) * 0.5f;
        // Rows carry their own band so real tracks stay distinct from the filler bands underneath.
        drawList->AddRectFilled(
            {layout.canvasLeft, std::max(y, layout.bodyTop)},
            {layout.fullMax.x, std::min(rowBottom, layout.bodyBottom)},
            bandIndex % 2 == 0 ? theme::kTimelineBg : theme::kTimelineTrackBg
        );
        drawList->AddLine({layout.canvasLeft, rowBottom}, {layout.fullMax.x, rowBottom}, IM_COL32(0, 0, 0, 64));
        ++bandIndex;

        if (row.cameraIndex >= 0 && row.cameraIndex < static_cast<int>(project->cameras.size())) {
            auto const& camera = project->cameras[row.cameraIndex];
            bool const  cameraSelected =
                (ctx.selection.getAs<state::editing::model::SelectedCamera>()
                 && ctx.selection.getAs<state::editing::model::SelectedCamera>()->cameraId == camera.id)
                || (selectedKeyframe && selectedKeyframe->trackId == camera.id);
            ImU32 const trackColor = theme::trackColor(row.cameraIndex);
            {
                float const amount = mAnimator.animate("canvas-selected", camera.id, cameraSelected ? 1.0f : 0.0f);
                ImU32 const fill   = camera.enabled ? lerpColor(
                                                          theme::withAlpha(trackColor, theme::kTrackFillAlpha),
                                                          theme::withAlpha(trackColor, theme::kTrackFillSelectedAlpha),
                                                          amount
                                                      )
                                                    : theme::withAlpha(theme::kTrackDisabled, 0x80);
                drawList->AddRectFilled({layout.canvasLeft, y + 3.0f}, {layout.fullMax.x, rowBottom - 3.0f}, fill);
            }

            for (auto const& [keyTick, key] : camera.keysByTick) {
                bool const  dragging  = mDraggingKeyframeCameraId == camera.id && mDraggingKeyframeStartTick == keyTick;
                int const   drawnTick = dragging ? mDraggingKeyframeTick : keyTick;
                float const x         = scale.xAt(static_cast<float>(drawnTick));
                if (x < layout.canvasLeft - keyRadius || x > layout.fullMax.x + keyRadius) continue;
                bool const selected =
                    selectedKeyframe && selectedKeyframe->trackId == camera.id && selectedKeyframe->tick == keyTick;
                bool const hold = key.interpolationType == state::editing::model::CameraInterpolationType::Hold;

                float const radius  = selected ? keyRadius + 1.0f : keyRadius;
                ImU32 const fill    = !camera.enabled ? theme::kKeyframeDisabled
                                    : selected        ? theme::kKeyframeSelected
                                                      : theme::kKeyframe;
                ImU32 const outline = selected ? theme::kIconHighlight : theme::kKeyframeOutline;
                if (hold) drawKeyframeSquare(drawList, x, centerY, radius, fill, outline);
                else drawKeyframeDiamond(drawList, x, centerY, radius, fill, outline);

                if (allowInput && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                    && std::abs(ImGui::GetMousePos().x - x) <= radius + 3.0f && ImGui::GetMousePos().y >= y
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
            if (row.kind == TrackRowKind::Camera && allowInput && !clickConsumed
                && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                && contains(
                    {layout.canvasLeft, y + 3.0f},
                    {layout.fullMax.x, rowBottom - 3.0f},
                    ImGui::GetMousePos()
                )) {
                clickConsumed = true;
                ctx.selection.select(state::editing::model::SelectedCamera{camera.id});
                EditorAction action{EditorActionType::SetPreviewCamera};
                action.id = camera.id;
                ctx.submitAction(std::move(action));
            }
        }
        y = rowBottom;
    }

    drawTrackGrid(ctx, layout, scale);
    drawExportRange(ctx, layout, scale, allowInput);

    if (allowInput && !mDraggingKeyframeCameraId.empty()) {
        if (std::abs(ImGui::GetMousePos().x - mDraggingKeyframeStartMouseX) > 2.0f) mDraggingKeyframeMoved = true;
        int const candidateTick =
            mDraggingKeyframeMoved ? boundedKeyframeTick(snapTick(tickFromMouse())) : mDraggingKeyframeStartTick;
        mDraggingKeyframeTick = candidateTick;
        float const markerX   = scale.xAt(static_cast<float>(candidateTick));
        drawList->AddLine(
            {markerX, layout.bodyTop},
            {markerX, layout.bodyBottom},
            theme::withAlpha(theme::kKeyframeSelected, 180),
            1.0f
        );
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

    float const playheadX = std::clamp(scale.xAt(static_cast<float>(displayTick)), layout.canvasLeft, layout.fullMax.x);
    if (allowInput && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && std::abs(ImGui::GetMousePos().x - playheadX) <= 6.0f && ImGui::GetMousePos().y >= layout.rulerTop
        && ImGui::GetMousePos().y < layout.bodyBottom) {
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
        && ImGui::GetMousePos().x >= layout.canvasLeft && ImGui::GetMousePos().x <= layout.fullMax.x
        && ImGui::GetMousePos().y >= layout.bodyTop && ImGui::GetMousePos().y < layout.bodyBottom) {
        submitSeek(ctx, tickFromMouse());
    }

    float const visiblePlayheadX =
        std::clamp(scale.xAt(static_cast<float>(displayTick)), layout.canvasLeft, layout.fullMax.x);
    drawList
        ->AddLine({visiblePlayheadX, layout.rulerTop}, {visiblePlayheadX, layout.bodyBottom}, theme::kPlayhead, 1.0f);
    {
        std::string const badgeText = formatTick(displayTick);
        ImVec2 const      textSize  = ImGui::CalcTextSize(badgeText.c_str());
        float const       badgeW    = textSize.x + 10.0f;
        float const       badgeH    = metrics::ruler() - 5.0f;
        float const badgeX = std::clamp(visiblePlayheadX - badgeW * 0.5f, layout.canvasLeft, layout.fullMax.x - badgeW);
        float const badgeY = layout.rulerTop + 2.0f;
        drawList->AddRectFilled({badgeX, badgeY}, {badgeX + badgeW, badgeY + badgeH}, theme::kPlayhead, 2.0f);
        drawList
            ->AddText({badgeX + 5.0f, badgeY + (badgeH - textSize.y) * 0.5f}, theme::kPlayheadLabel, badgeText.c_str());
    }
    drawList->PopClipRect();

    if (maxScrollY > 0.0f) {
        float const trackHeight = std::max(1.0f, visibleTrackHeight);
        float const thumbHeight = std::max(20.0f, trackHeight * trackHeight / std::max(1.0f, trackContentHeight));
        float const thumbTop    = layout.bodyTop + (trackHeight - thumbHeight) * (mScrollY / maxScrollY);
        drawList->AddRectFilled(
            {layout.fullMax.x - 5.0f, thumbTop},
            {layout.fullMax.x - 2.0f, thumbTop + thumbHeight},
            theme::withAlpha(theme::kScrollThumb, 200),
            2.0f
        );
    }

    ImVec2 const wheelMouse = ImGui::GetMousePos();
    bool const   canvasHovered =
        contains({layout.canvasLeft, layout.rulerTop}, {layout.fullMax.x, layout.bodyBottom}, wheelMouse);
    bool const trackListHovered = wheelMouse.x >= fullMin.x && wheelMouse.x < layout.canvasLeft
                               && wheelMouse.y >= layout.bodyTop && wheelMouse.y < layout.bodyBottom;
    if (allowInput && (canvasHovered || trackListHovered) && ImGui::GetIO().MouseWheel != 0.0f) {
        float const wheel = ImGui::GetIO().MouseWheel;
        if (ImGui::GetIO().KeyAlt) {
            float const anchorX    = std::clamp(wheelMouse.x - layout.canvasLeft, 0.0f, layout.canvasWidth);
            float const anchorTick = scale.tickAtExact(layout.canvasLeft + anchorX);
            mZoomScale =
                std::clamp(mZoomScale * (wheel > 0.0f ? kZoomStep : 1.0f / kZoomStep), kMinZoomScale, kMaxZoomScale);
            float const nextPixelsPerTick = fitPixelsPerTick * mZoomScale;
            float const nextMaxScroll     = std::max(0.0f, state.totalTicks * nextPixelsPerTick - layout.canvasWidth);
            mScrollX                      = std::clamp(anchorTick * nextPixelsPerTick - anchorX, 0.0f, nextMaxScroll);
        } else if (canvasHovered && maxScroll > 0.0f) {
            mScrollX = std::clamp(mScrollX - wheel * 60.0f, 0.0f, maxScroll);
        } else {
            mScrollY = std::clamp(mScrollY - wheel * 45.0f, 0.0f, maxScrollY);
        }
    }

    drawRangeBar(ctx, layout, allowInput);
}

} // namespace playback::editor::ui
