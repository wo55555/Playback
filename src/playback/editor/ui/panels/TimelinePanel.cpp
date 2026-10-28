#include "TimelinePanel.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/PlayerPicker.h"
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

// Resolves a target track so the shortcut works without a prior selection.
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
    } else if (auto const* selectedSegment = ctx.selection.getAs<state::editing::model::SelectedSequenceSegment>();
               selectedSegment && project->sequence.size() > 1) {
        action.type = EditorActionType::DeleteSequenceSegment;
        action.id   = selectedSegment->segmentId;
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
    float const startX = std::max(0.0f, (width - groupWidth) * 0.5f);

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
        widgets::openAnchoredPopup("##transport-speed-menu");
    }
    // The menu lists the speeds themselves; stepping up and down stays on the keyboard shortcuts.
    if (widgets::beginAnchoredPopup("##transport-speed-menu")) {
        for (float const speed : replay::ReplaySession::playbackSpeeds()) {
            char label[16]{};
            std::snprintf(label, sizeof(label), "%gx", static_cast<double>(speed));
            if (ImGui::MenuItem(label, nullptr, std::abs(state.playbackSpeed - speed) < 0.0001f)) {
                EditorAction action{EditorActionType::SetPlaybackSpeed};
                action.speed = speed;
                ctx.submitAction(std::move(action));
            }
        }
        widgets::endAnchoredPopup();
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
        widgets::openAnchoredPopup("##timeline-add-menu");
    if (widgets::beginAnchoredPopup("##timeline-add-menu")) {
        // An empty name lets the ops layer pick the next free localized default.
        if (ImGui::MenuItem("playback.refactorEditor.details.addFreeCamera"_tr().c_str())) {
            ctx.submitAction({EditorActionType::AddFreeCamera});
        }
        // One switch lane per project: it spans the replay and is split into per-camera ranges.
        if (ImGui::MenuItem(
                "playback.refactorEditor.timeline.addSequenceTrack"_tr().c_str(),
                nullptr,
                false,
                project && project->sequence.empty()
            )) {
            ctx.submitAction({EditorActionType::AddCameraSequence});
        }
        widgets::endAnchoredPopup();
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
    bool const  sequenceSelected = ctx.selection.getAs<state::editing::model::SelectedSequence>()
                               || ctx.selection.getAs<state::editing::model::SelectedSequenceSegment>();
    float       rowY             = layout.bodyTop - mScrollY;
    for (auto const& row : mTrackTree.rows()) {
        float const rowBottom = rowY + row.height;
        if (rowBottom < layout.bodyTop || rowY > layout.bodyBottom) {
            rowY = rowBottom;
            continue;
        }
        bool const sequenceRow = row.kind == TrackRowKind::Sequence;
        bool const selected    = sequenceRow ? sequenceSelected
                                             : !row.cameraId.empty()
                                            && ((selectedCamera && selectedCamera->cameraId == row.cameraId)
                                                || (selectedKeyframe && selectedKeyframe->trackId == row.cameraId));

        ImGui::SetCursorScreenPos({listLeft, rowY});
        ImGui::InvisibleButton(("##track-row-" + row.id).c_str(), {listW, row.height});
        bool const  hovered        = ImGui::IsItemHovered();
        bool const  clicked        = allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Left);
        bool const  rowRightClicked = allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Right);
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
        ImU32 const swatch = sequenceRow  ? theme::kAccent
                           : row.enabled ? theme::trackColor(row.cameraIndex)
                                         : theme::kTrackDisabled;
        drawList->AddRectFilled(
            {listLeft, rowY},
            {listLeft + metrics::trackSwatch(), rowBottom},
            lerpColor(theme::withAlpha(swatch, 0x99), swatch, selectedAmount)
        );

        float const indent = metrics::gutter() + metrics::trackSwatch();
        float       labelX = listLeft + indent;

        ImU32 const textColor = row.enabled ? theme::kText : theme::kTextDim;
        std::string label     = sequenceRow ? "playback.refactorEditor.timeline.sequenceTrack"_tr() : row.name;
        // Text centres on the fractional row, then the icon centres on the text's optical middle.
        float const rowCentre = (rowY + rowBottom) * 0.5f;
        float const textY     = widgets::textYForCentre(rowCentre, label.c_str());
        float const iconBox   = metrics::iconGlyph();
        widgets::drawIconAtCentre(
            drawList,
            sequenceRow ? ICON_TRANSITION : ICON_CAMERA,
            {labelX + iconBox * 0.5f, widgets::textInkCentre(textY, label.c_str())},
            textColor
        );
        labelX += iconBox + metrics::gutter() * 0.5f;
        drawList->AddText({labelX, textY}, textColor, label.c_str());

        if (sequenceRow) {
            float const  controlWidth = metrics::iconGlyph();
            float const  deleteX      = listLeft + listW - controlWidth - metrics::gutter();
            ImVec2 const mouse        = ImGui::GetMousePos();
            bool const   deleteHovered = contains({deleteX, rowY}, {deleteX + controlWidth, rowBottom}, mouse);
            if (deleteHovered) widgets::hoverTooltip("playback.refactorEditor.timeline.deleteSequenceTrack"_tr().c_str());
            if (allowInput && deleteHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ctx.submitAction({EditorActionType::DeleteCameraSequence});
                ctx.selection.clear();
            } else if (clicked) {
                ctx.selection.select(state::editing::model::SelectedSequence{});
            } else if (rowRightClicked && !deleteHovered) {
                ctx.selection.select(state::editing::model::SelectedSequence{});
                mTrackMenuSequence = true;
                widgets::openAnchoredPopup("##timeline-track-menu");
            }
            widgets::drawIconAtCentre(
                drawList,
                ICON_TRASH,
                {deleteX + controlWidth * 0.5f, rowCentre},
                deleteHovered ? theme::kIconHighlight : theme::kIconInactive
            );
        }

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
            } else if (rowRightClicked && mouse.x < eyeX) {
                ctx.selection.select(state::editing::model::SelectedCamera{row.cameraId});
                mTrackMenuCameraId = row.cameraId;
                mTrackMenuSequence = false;
                widgets::openAnchoredPopup("##timeline-track-menu");
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
    // Unset means "whole replay", so nothing is drawn until the user marks a point.
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

void TimelinePanel::drawSequenceLane(
    PanelContext const&  ctx,
    Layout const&        layout,
    TimelineScale const& scale,
    float                top,
    float                bottom,
    bool                 allowInput,
    bool&                clickConsumed
) {
    auto const project = ctx.state.project;
    if (!project || project->sequence.empty()) return;
    auto const& segments = project->sequence;
    auto*       drawList = ImGui::GetWindowDrawList();
    ImVec2 const mouse   = ImGui::GetMousePos();
    float const  inset   = 3.0f * metrics::scale();
    float const  edgeHit = 4.0f * metrics::scale();
    bool const   inLane  = mouse.y >= top + inset && mouse.y <= bottom - inset && mouse.x >= layout.canvasLeft
                        && mouse.x <= layout.fullMax.x;
    auto const* selected = ctx.selection.getAs<state::editing::model::SelectedSequenceSegment>();

    if (!allowInput) mSequenceDragId.clear();
    // Only the boundary between two segments moves; the outer ends are pinned to the replay start and end.
    auto const dragIndex = [&]() -> int {
        if (mSequenceDragId.empty()) return -1;
        auto const it = std::ranges::find(segments, mSequenceDragId, &state::editing::model::SequenceSegment::id);
        return it == segments.end() || std::next(it) == segments.end() ? -1
                                                                         : static_cast<int>(it - segments.begin());
    }();
    if (dragIndex < 0) mSequenceDragId.clear();

    auto const boundaryTick = [&](size_t index, bool end) {
        int tick = end ? segments[index].endTick : segments[index].startTick;
        if (dragIndex >= 0) {
            if (end && static_cast<int>(index) == dragIndex) tick = mSequenceDragTick;
            if (!end && static_cast<int>(index) == dragIndex + 1) tick = mSequenceDragTick;
        }
        return tick;
    };

    bool hoverEdge = false;
    for (size_t index = 0; index < segments.size(); ++index) {
        auto const& segment = segments[index];
        float const left    = std::max(layout.canvasLeft, scale.xAt(static_cast<float>(boundaryTick(index, false))));
        float const right   = std::min(layout.fullMax.x, scale.xAt(static_cast<float>(boundaryTick(index, true))));
        if (right <= left) continue;

        std::string label;
        ImU32       color = theme::kAutoSegment;
        if (segment.cameraId == state::editing::model::kFirstPersonCameraId) {
            label = "playback.refactorEditor.timeline.firstPersonCamera"_tr();
            color = theme::kFirstPersonSegment;
        } else if (auto const camera =
                       std::ranges::find(project->cameras, segment.cameraId, &state::editing::model::CameraEntity::id);
                   !segment.cameraId.empty() && camera != project->cameras.end()) {
            label = camera->name;
            color = camera->enabled ? theme::trackColor(static_cast<int>(camera - project->cameras.begin()))
                                    : theme::kTrackDisabled;
        } else {
            label = "playback.refactorEditor.timeline.autoCamera"_tr();
        }

        bool const  isSelected = selected && selected->segmentId == segment.id;
        float const amount     = mAnimator.animate("sequence-selected", segment.id, isSelected ? 1.0f : 0.0f);
        ImVec2 const minimum{left + 1.0f, top + inset};
        ImVec2 const maximum{right - 1.0f, bottom - inset};
        float const  rounding = 3.0f * metrics::scale();
        drawList->AddRectFilled(
            minimum,
            maximum,
            lerpColor(theme::withAlpha(color, 0x70), theme::withAlpha(color, 0xb0), amount),
            rounding
        );
        drawList->AddRect(
            minimum,
            maximum,
            lerpColor(theme::withAlpha(color, 0xd0), theme::kIconHighlight, amount),
            rounding,
            0,
            1.0f
        );
        float const textY = widgets::textYForCentre((top + bottom) * 0.5f, label.c_str());
        drawList->PushClipRect(minimum, maximum, true);
        drawList->AddText({minimum.x + metrics::gutter(), textY}, theme::kText, label.c_str());
        drawList->PopClipRect();

        // The right edge of every segment but the last is a shared boundary that trims both neighbours.
        bool const movable = index + 1 < segments.size() && !segment.locked && !segments[index + 1].locked;
        bool const nearEdge = movable && inLane && std::abs(mouse.x - right) <= edgeHit;
        if (nearEdge || (dragIndex == static_cast<int>(index))) {
            hoverEdge = true;
            drawList->AddLine({right, top + inset}, {right, bottom - inset}, theme::kIconHighlight, 2.0f);
        }
        if (allowInput && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && nearEdge) {
            clickConsumed     = true;
            mSequenceDragId   = segment.id;
            mSequenceDragEnd  = true;
            mSequenceDragTick = segment.endTick;
            continue;
        }
        bool const overSegment = allowInput && !clickConsumed && inLane && mouse.x > left && mouse.x < right;
        bool const openMenu    = overSegment
                              && ((ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !nearEdge)
                                  || ImGui::IsMouseClicked(ImGuiMouseButton_Right));
        if (openMenu) {
            clickConsumed = true;
            ctx.selection.select(state::editing::model::SelectedSequenceSegment{segment.id});
            // The preview follows the switch lane only while no single camera is pinned.
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ctx.submitAction({EditorActionType::ClearPreviewCamera});
            mSequenceMenuSegmentId = segment.id;
            mSequenceMenuTick      = std::clamp(scale.tickAt(mouse.x), 0, ctx.state.totalTicks);
            widgets::openAnchoredPopup("##sequence-segment-menu", minimum, maximum);
        }
    }
    if (hoverEdge) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    if (dragIndex < 0) return;
    auto const& left  = segments[static_cast<size_t>(dragIndex)];
    auto const& right = segments[static_cast<size_t>(dragIndex) + 1];
    int         tick  = std::clamp(scale.tickAt(mouse.x), 0, ctx.state.totalTicks);
    if (mSnapEnabled) tick = static_cast<int>(std::round(static_cast<float>(tick) / kTicksPerSecond)) * kTicksPerSecond;
    // Both neighbours keep at least one tick.
    mSequenceDragTick = std::clamp(tick, left.startTick + 1, std::max(left.startTick + 1, right.endTick - 1));
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    if (mSequenceDragTick != left.endTick) {
        EditorAction action{EditorActionType::TrimSequence};
        action.id   = left.id;
        action.tick = left.startTick;
        action.kind = mSequenceDragTick;
        ctx.submitAction(std::move(action));
    }
    mSequenceDragId.clear();
}

void TimelinePanel::drawContextMenus(PanelContext const& ctx) {
    auto const project = ctx.state.project;
    using CameraInterpolationType = state::editing::model::CameraInterpolationType;

    if (widgets::beginAnchoredPopup("##timeline-keyframe-menu")) {
        state::editing::model::CameraEntity const* camera = nullptr;
        if (project) {
            auto const found =
                std::ranges::find(project->cameras, mKeyframeMenuCameraId, &state::editing::model::CameraEntity::id);
            if (found != project->cameras.end()) camera = &*found;
        }
        auto const key = [&]() -> state::editing::model::CameraKeyframe const* {
            if (!camera) return nullptr;
            auto const found = camera->keysByTick.find(mKeyframeMenuTick);
            return found == camera->keysByTick.end() ? nullptr : &found->second;
        }();
        if (!camera || !key) {
            ImGui::CloseCurrentPopup();
        } else {
            bool const editable = !camera->locked;
            auto const setInterpolation = [&](char const* label, CameraInterpolationType type) {
                if (ImGui::MenuItem(label, nullptr, key->interpolationType == type, editable)) {
                    EditorAction action{EditorActionType::SetKeyframeInterpolation};
                    action.id   = camera->id;
                    action.tick = mKeyframeMenuTick;
                    action.kind = static_cast<int>(type);
                    ctx.submitAction(std::move(action));
                }
            };
            if (ImGui::BeginMenu("playback.refactorEditor.details.interpolation"_tr().c_str(), editable)) {
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.smooth"_tr().c_str(),
                    CameraInterpolationType::Smooth
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.linear"_tr().c_str(),
                    CameraInterpolationType::Linear
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.hold"_tr().c_str(),
                    CameraInterpolationType::Hold
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.easeIn"_tr().c_str(),
                    CameraInterpolationType::EaseIn
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.easeOut"_tr().c_str(),
                    CameraInterpolationType::EaseOut
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.easeInOut"_tr().c_str(),
                    CameraInterpolationType::EaseInOut
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.hermite"_tr().c_str(),
                    CameraInterpolationType::Hermite
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.cubicBezier"_tr().c_str(),
                    CameraInterpolationType::CubicBezier
                );
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("playback.refactorEditor.timeline.copyKeyframe"_tr().c_str())) {
                mKeyframeClipboard = *key;
            }

            ImGui::SetNextItemWidth(96.0f * metrics::scale());
            ImGui::BeginDisabled(!editable);
            ImGui::InputInt("##timeline-keyframe-tick", &mKeyframeMenuTickEdit, 0, 0);
            bool const tickCommitted = ImGui::IsItemDeactivatedAfterEdit();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", "playback.refactorEditor.timeline.setTick"_tr().c_str());
            if (editable && tickCommitted) {
                int const next = std::clamp(mKeyframeMenuTickEdit, 0, ctx.state.totalTicks);
                if (next != mKeyframeMenuTick && !camera->keysByTick.contains(next)) {
                    EditorAction action{EditorActionType::MoveCameraKeyframe};
                    action.id            = camera->id;
                    action.tick          = mKeyframeMenuTick;
                    action.secondaryTick = next;
                    ctx.submitAction(std::move(action));
                    ctx.selection.select(state::editing::model::SelectedKeyframe{camera->id, next});
                    mKeyframeMenuTick     = next;
                    mKeyframeMenuTickEdit = next;
                } else {
                    mKeyframeMenuTickEdit = mKeyframeMenuTick;
                }
            }

            if (ImGui::MenuItem("playback.refactorEditor.timeline.seekHere"_tr().c_str())) seekTo(ctx, mKeyframeMenuTick);
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.previewCamera"_tr().c_str(),
                    nullptr,
                    false,
                    state::editing::model::isCameraRenderable(*camera)
                )) {
                EditorAction action{EditorActionType::SetPreviewCamera};
                action.id = camera->id;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.details.deleteKeyframe"_tr().c_str(),
                    nullptr,
                    false,
                    editable
                )) {
                EditorAction action{EditorActionType::DeleteCameraKeyframe};
                action.id   = camera->id;
                action.tick = mKeyframeMenuTick;
                ctx.submitAction(std::move(action));
                ctx.selection.clear();
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-lane-menu")) {
        state::editing::model::CameraEntity const* camera = nullptr;
        if (project) {
            auto const found =
                std::ranges::find(project->cameras, mLaneMenuCameraId, &state::editing::model::CameraEntity::id);
            if (found != project->cameras.end()) camera = &*found;
        }
        if (!camera) {
            ImGui::CloseCurrentPopup();
        } else {
            bool const editable = !camera->locked;
            bool const occupied = camera->keysByTick.contains(mLaneMenuTick);
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.addKeyframeHere"_tr().c_str(),
                    nullptr,
                    false,
                    editable && !occupied
                )) {
                EditorAction action{EditorActionType::AddCameraKeyframe};
                action.id   = camera->id;
                action.tick = mLaneMenuTick;
                ctx.submitAction(std::move(action));
                ctx.selection.select(state::editing::model::SelectedKeyframe{camera->id, mLaneMenuTick});
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.pasteKeyframe"_tr().c_str(),
                    nullptr,
                    false,
                    editable && !occupied && mKeyframeClipboard.has_value()
                )) {
                EditorAction action{EditorActionType::AddCameraKeyframe};
                action.id       = camera->id;
                action.tick     = mLaneMenuTick;
                action.keyframe = mKeyframeClipboard;
                ctx.submitAction(std::move(action));
                ctx.selection.select(state::editing::model::SelectedKeyframe{camera->id, mLaneMenuTick});
            }
            if (ImGui::MenuItem("playback.refactorEditor.timeline.seekHere"_tr().c_str())) seekTo(ctx, mLaneMenuTick);
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-track-menu")) {
        if (mTrackMenuSequence) {
            if (ImGui::MenuItem("playback.refactorEditor.timeline.deleteSequenceTrack"_tr().c_str())) {
                ctx.submitAction({EditorActionType::DeleteCameraSequence});
                ctx.selection.clear();
            }
        } else {
            state::editing::model::CameraEntity const* camera = nullptr;
            if (project) {
                auto const found =
                    std::ranges::find(project->cameras, mTrackMenuCameraId, &state::editing::model::CameraEntity::id);
                if (found != project->cameras.end()) camera = &*found;
            }
            if (!camera) {
                ImGui::CloseCurrentPopup();
            } else {
            if (ImGui::MenuItem(
                    (camera->enabled ? "playback.refactorEditor.timeline.disableTrack"_tr()
                                     : "playback.refactorEditor.timeline.enableTrack"_tr())
                        .c_str()
                )) {
                EditorAction action{EditorActionType::SetCameraEnabled};
                action.id    = camera->id;
                action.value = !camera->enabled;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.previewCamera"_tr().c_str(),
                    nullptr,
                    false,
                    state::editing::model::isCameraRenderable(*camera)
                )) {
                EditorAction action{EditorActionType::SetPreviewCamera};
                action.id = camera->id;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.deleteTrack"_tr().c_str(),
                    nullptr,
                    false,
                    !camera->locked && project->cameras.size() > 1
                )) {
                EditorAction action{EditorActionType::DeleteCamera};
                action.id = camera->id;
                ctx.submitAction(std::move(action));
                ctx.selection.clear();
            }
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-ruler-menu")) {
        int const total = ctx.state.totalTicks;
        if (ImGui::MenuItem("playback.refactorEditor.timeline.seekHere"_tr().c_str())) seekTo(ctx, mRulerMenuTick);
        if (ImGui::MenuItem("playback.refactorEditor.timeline.setInPoint"_tr().c_str(), nullptr, false, total > 0)) {
            int const tick = std::clamp(mRulerMenuTick, 0, std::max(0, total - 1));
            int       out  = ctx.commands.exportEndTick();
            if (out <= tick) out = total;
            ctx.commands.setExportStartTick(tick);
            ctx.commands.setExportEndTick(out);
        }
        if (ImGui::MenuItem("playback.refactorEditor.timeline.setOutPoint"_tr().c_str(), nullptr, false, total > 0)) {
            int const tick = std::clamp(mRulerMenuTick, std::min(1, total), total);
            int       in   = ctx.commands.exportStartTick();
            if (in < 0 || in >= tick) in = 0;
            ctx.commands.setExportStartTick(in);
            ctx.commands.setExportEndTick(tick);
        }
        bool const ranged = ctx.commands.exportStartTick() >= 0 || ctx.commands.exportEndTick() >= 0;
        if (ImGui::MenuItem("playback.refactorEditor.timeline.clearExportRange"_tr().c_str(), nullptr, false, ranged)) {
            ctx.commands.setExportStartTick(-1);
            ctx.commands.setExportEndTick(-1);
        }
        widgets::endAnchoredPopup();
    }
}

void TimelinePanel::drawSequenceSegmentMenu(PanelContext const& ctx) {
    if (!widgets::beginAnchoredPopup("##sequence-segment-menu")) return;
    auto const project = ctx.state.project;
    if (!project) {
        ImGui::CloseCurrentPopup();
        widgets::endAnchoredPopup();
        return;
    }
    auto const segment =
        std::ranges::find(project->sequence, mSequenceMenuSegmentId, &state::editing::model::SequenceSegment::id);
    // An undo can remove the segment while its menu is open.
    if (segment == project->sequence.end()) {
        ImGui::CloseCurrentPopup();
        widgets::endAnchoredPopup();
        return;
    }

    auto const bind = [&](std::string cameraId) {
        EditorAction action{EditorActionType::BindSequenceCamera};
        action.id          = segment->id;
        action.secondaryId = std::move(cameraId);
        ctx.submitAction(std::move(action));
    };
    bool const editable = !segment->locked;
    ImGui::TextDisabled("%s", "playback.refactorEditor.timeline.sequenceCameraHeader"_tr().c_str());
    if (ImGui::MenuItem(
            "playback.refactorEditor.timeline.autoCamera"_tr().c_str(),
            nullptr,
            segment->cameraId.empty(),
            editable
        )) {
        bind({});
    }
    if (ImGui::MenuItem(
            "playback.refactorEditor.timeline.firstPersonCamera"_tr().c_str(),
            nullptr,
            segment->cameraId == state::editing::model::kFirstPersonCameraId,
            editable
        )) {
        bind(std::string{state::editing::model::kFirstPersonCameraId});
    }
    if (segment->cameraId == state::editing::model::kFirstPersonCameraId) {
        std::string const subject = playerSubjectLabel(ctx.state.replayPlayers, ctx.state.replaySubjectChoice);
        ImGui::TextDisabled("%s", "playback.refactorEditor.timeline.firstPersonSubject"_tr(subject).c_str());
        if (ImGui::IsItemHovered()) {
            widgets::hoverTooltip("playback.refactorEditor.timeline.subjectShared"_tr().c_str());
        }
        // The subject list lives in the inspector, where it has room for full names.
        if (ImGui::MenuItem("playback.refactorEditor.timeline.changeSubject"_tr().c_str())) {
            ImGui::CloseCurrentPopup();
            ctx.selection.select(state::editing::model::SelectedSequenceSegment{segment->id});
            ctx.commands.focusFirstPersonSubject();
        }
    }
    if (!project->cameras.empty()) ImGui::Separator();
    for (auto const& camera : project->cameras) {
        ImGui::PushID(camera.id.c_str());
        if (ImGui::MenuItem(camera.name.c_str(), nullptr, segment->cameraId == camera.id, editable)) bind(camera.id);
        ImGui::PopID();
    }

    ImGui::Separator();
    int const  playhead = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    bool const canSplit = std::ranges::any_of(project->sequence, [&](auto const& value) {
        return playhead > value.startTick && playhead < value.endTick && !value.locked;
    });
    bool const canSplitHere =
        editable && mSequenceMenuTick > segment->startTick && mSequenceMenuTick < segment->endTick;
    if (ImGui::MenuItem("playback.refactorEditor.timeline.splitHere"_tr().c_str(), nullptr, false, canSplitHere)) {
        EditorAction action{EditorActionType::SplitSequence};
        action.tick = mSequenceMenuTick;
        ctx.submitAction(std::move(action));
    }
    auto const index = static_cast<size_t>(segment - project->sequence.begin());
    bool const canMergePrevious = index > 0 && editable && !project->sequence[index - 1].locked;
    bool const canMergeNext =
        index + 1 < project->sequence.size() && editable && !project->sequence[index + 1].locked;
    if (ImGui::MenuItem(
            "playback.refactorEditor.timeline.mergeWithPrevious"_tr().c_str(),
            nullptr,
            false,
            canMergePrevious
        )) {
        EditorAction action{EditorActionType::MergeSequenceSegment};
        action.id = segment->id;
        ctx.submitAction(std::move(action));
        ctx.selection.clear();
    }
    if (ImGui::MenuItem(
            "playback.refactorEditor.timeline.mergeWithNext"_tr().c_str(),
            nullptr,
            false,
            canMergeNext
        )) {
        EditorAction action{EditorActionType::MergeSequenceSegment};
        action.id   = segment->id;
        action.kind = 1;
        ctx.submitAction(std::move(action));
    }
    if (ImGui::MenuItem("playback.refactorEditor.timeline.splitAtPlayhead"_tr().c_str(), nullptr, false, canSplit)) {
        EditorAction action{EditorActionType::SplitSequence};
        action.tick = playhead;
        ctx.submitAction(std::move(action));
    }
    if (ImGui::MenuItem(
            "playback.refactorEditor.timeline.deleteSegment"_tr().c_str(),
            nullptr,
            false,
            editable && project->sequence.size() > 1
        )) {
        EditorAction action{EditorActionType::DeleteSequenceSegment};
        action.id = segment->id;
        ctx.submitAction(std::move(action));
        ctx.selection.clear();
    }
    widgets::endAnchoredPopup();
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

    // The thumb is the visible window over the whole replay: its end handles scale, the body pans.
    float const windowSpan  = std::clamp(1.0f / std::max(kMinZoomScale, mZoomScale), 0.02f, 1.0f);
    float const maxScroll   = maxScrollFor(layout.canvasWidth, mZoomScale, state.totalTicks);
    float const windowStart = maxScroll > 0.0f ? (mScrollX / maxScroll) * (1.0f - windowSpan) : 0.0f;
    float const uiScale     = metrics::scale();
    float const centreY     = (barTop + layout.fullMax.y) * 0.5f;
    // Premiere style: a thin bar whose ends carry ring handles centred on the window edges.
    float const radius    = std::max(4.0f, std::min(barHeight, metrics::rangeBar()) * 0.26f);
    float const thumbH    = std::max(3.0f, radius * 1.1f);
    float const top       = centreY - thumbH * 0.5f;
    float const bottom    = centreY + thumbH * 0.5f;
    float const rounding  = thumbH * 0.5f;
    float const ring      = std::max(1.5f, 1.5f * uiScale);
    float const handleW   = radius + 2.0f * uiScale;
    float const minThumb  = handleW * 2.0f + 6.0f * uiScale;
    float const railLeft  = barLeft + radius + ring;
    float const railWidth = barWidth - (radius + ring) * 2.0f;
    if (railWidth <= minThumb) {
        mRangeDragMode = 0;
        return;
    }

    // Deep zoom still draws the thumb wide enough to hold both handles.
    float winLeft  = railLeft + windowStart * railWidth;
    float winRight = winLeft + windowSpan * railWidth;
    if (winRight - winLeft < minThumb) {
        float const centre =
            std::clamp((winLeft + winRight) * 0.5f, railLeft + minThumb * 0.5f, railLeft + railWidth - minThumb * 0.5f);
        winLeft  = centre - minThumb * 0.5f;
        winRight = centre + minThumb * 0.5f;
    }

    ImGui::SetCursorScreenPos({barLeft, barTop});
    ImGui::InvisibleButton("##timeline-range", {barWidth, barHeight});
    ImVec2 const mouse      = ImGui::GetMousePos();
    bool const   barHovered = ImGui::IsItemHovered();
    bool const   nearLeft   = barHovered && std::abs(mouse.x - winLeft) <= handleW;
    bool const   nearRight  = barHovered && !nearLeft && std::abs(mouse.x - winRight) <= handleW;
    bool const   overBody   = barHovered && !nearLeft && !nearRight && mouse.x > winLeft && mouse.x < winRight;
    bool const   idle       = mRangeDragMode == 0;
    float const  leftAmount =
        mAnimator.animate("range-bar", "left", mRangeDragMode == 1 || (idle && nearLeft) ? 1.0f : 0.0f);
    float const rightAmount =
        mAnimator.animate("range-bar", "right", mRangeDragMode == 2 || (idle && nearRight) ? 1.0f : 0.0f);
    // Half way is hover, full is dragging, so each state gets its own colour.
    float const bodyAmount =
        mAnimator.animate("range-bar", "body", mRangeDragMode == 3 ? 1.0f : (idle && overBody ? 0.5f : 0.0f));

    drawList->AddRectFilled({railLeft, top}, {railLeft + railWidth, bottom}, theme::kRangeTrack, rounding);
    ImU32 const thumbColor = bodyAmount <= 0.5f
                               ? lerpColor(theme::kRangeThumb, theme::kRangeThumbHover, bodyAmount * 2.0f)
                               : lerpColor(theme::kRangeThumbHover, theme::kRangeThumbActive, bodyAmount * 2.0f - 1.0f);
    drawList->AddRectFilled({winLeft, top}, {winRight, bottom}, thumbColor, rounding);

    // Hollow rings: the panel colour fills the centre so the bar appears to pass behind them.
    auto const handle = [&](float centreX, float amount) {
        float const r = radius * (1.0f + 0.15f * amount);
        drawList->AddCircleFilled({centreX, centreY}, r, theme::kBgHeader);
        drawList->AddCircle(
            {centreX, centreY},
            r - ring * 0.5f,
            lerpColor(theme::kRangeHandle, theme::kIconHighlight, amount),
            0,
            ring
        );
    };
    handle(winLeft, leftAmount);
    handle(winRight, rightAmount);

    if (!allowInput) {
        mRangeDragMode = 0;
        return;
    }
    if (barHovered || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGui::IsItemActive() ? theme::kCursorGrabbing : theme::kCursorGrab);
    }
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

    // The band is the gap between the track list and the canvas; the child windows on either side would take its hover.
    ImVec2 const listSplitterMin{fullMin.x + layout.listWidth, layout.rulerTop};
    ImVec2 const listSplitterMax{layout.canvasLeft, layout.bodyBottom};
    ImGui::SetCursorScreenPos(listSplitterMin);
    ImGui::InvisibleButton(
        "##timeline-list-splitter",
        {listSplitterMax.x - listSplitterMin.x, listSplitterMax.y - listSplitterMin.y}
    );
    bool const listSplitterHovered = ImGui::IsItemHovered();
    bool const listSplitterActive  = ImGui::IsItemActive();
    if (listSplitterHovered || listSplitterActive) {
        ImGui::SetMouseCursor(listSplitterActive ? theme::kCursorGrabbing : theme::kCursorGrab);
    }
    if (allowInput && listSplitterActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        float const listRight = ImGui::GetMousePos().x - metrics::splitter() * 0.5f;
        mTrackListWidthRatio  = std::clamp((listRight - fullMin.x) / available.x, 0.18f, 0.55f);
    }

    drawTrackHeaders(ctx, layout, allowInput);
    if (listSplitterHovered || listSplitterActive) {
        drawList->AddRectFilled(
            listSplitterMin,
            listSplitterMax,
            listSplitterActive ? theme::kAccent : theme::withAlpha(theme::kAccent, 0xa0)
        );
    }

    drawList
        ->AddRectFilled({layout.canvasLeft, layout.bodyTop}, {layout.fullMax.x, layout.bodyBottom}, theme::kTimelineBg);
    drawList->PushClipRect({layout.canvasLeft, layout.rulerTop}, {layout.fullMax.x, layout.bodyBottom}, true);
    drawRuler(ctx, layout, scale, displayTick);

    ImGui::SetCursorScreenPos({layout.canvasLeft, layout.rulerTop});
    ImGui::InvisibleButton("##timeline-ruler", {layout.canvasWidth, layout.bodyTop - layout.rulerTop});
    if (allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        mRulerMenuTick = std::clamp(scale.tickAt(ImGui::GetMousePos().x), 0, state.totalTicks);
        ImVec2 const anchor{scale.xAt(static_cast<float>(mRulerMenuTick)), layout.rulerTop};
        widgets::openAnchoredPopup("##timeline-ruler-menu", anchor, {anchor.x, layout.bodyTop});
    }
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

        if (row.kind == TrackRowKind::Sequence) {
            drawSequenceLane(ctx, layout, scale, y, rowBottom, allowInput, clickConsumed);
            y = rowBottom;
            continue;
        }

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
                ImVec2 const keyMouse = ImGui::GetMousePos();
                bool const   overKey  = std::abs(keyMouse.x - x) <= radius + 3.0f && keyMouse.y >= y
                                   && keyMouse.y <= rowBottom;
                if (allowInput && !clickConsumed && overKey && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    clickConsumed            = true;
                    mKeyframeMenuCameraId    = camera.id;
                    mKeyframeMenuTick        = keyTick;
                    mKeyframeMenuTickEdit    = keyTick;
                    ctx.selection.select(state::editing::model::SelectedKeyframe{camera.id, keyTick});
                    widgets::openAnchoredPopup(
                        "##timeline-keyframe-menu",
                        {x - radius, centerY - radius},
                        {x + radius, centerY + radius}
                    );
                }
                if (allowInput && !clickConsumed && overKey && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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
                && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
                && contains(
                    {layout.canvasLeft, y + 3.0f},
                    {layout.fullMax.x, rowBottom - 3.0f},
                    ImGui::GetMousePos()
                )) {
                clickConsumed = true;
                int tick      = std::clamp(scale.tickAt(ImGui::GetMousePos().x), 0, state.totalTicks);
                if (mSnapEnabled) {
                    tick = static_cast<int>(std::round(static_cast<float>(tick) / kTicksPerSecond)) * kTicksPerSecond;
                    tick = std::clamp(tick, 0, state.totalTicks);
                }
                mLaneMenuCameraId = camera.id;
                mLaneMenuTick     = tick;
                float const anchorX = scale.xAt(static_cast<float>(tick));
                widgets::openAnchoredPopup(
                    "##timeline-lane-menu",
                    {anchorX, y + 3.0f},
                    {anchorX, rowBottom - 3.0f}
                );
            }
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
    drawSequenceSegmentMenu(ctx);
    drawContextMenus(ctx);

    if (allowInput && !mDraggingKeyframeCameraId.empty()) {
        // Any horizontal movement latches into a drag, so a plain click only seeks.
        if (ImGui::GetMousePos().x != mDraggingKeyframeStartMouseX) mDraggingKeyframeMoved = true;
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
