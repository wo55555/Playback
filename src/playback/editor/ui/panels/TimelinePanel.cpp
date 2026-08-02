#include "TimelinePanel.h"

#include "playback/editor/ui/ReplayEditor.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace playback::editor::ui {

using namespace ll::i18n_literals;

namespace {

constexpr float kToolbarHeight     = 38.0f;
constexpr float kTransportHeight   = 34.0f;
constexpr float kRulerHeight       = 28.0f;
constexpr float kSplitterThickness = 4.0f;

std::string formatTick(int tick) {
    char value[32]{};
    tick = std::max(0, tick);
    std::snprintf(value, sizeof(value), "%02d:%02d", tick / 1200, (tick / 20) % 60);
    return value;
}

int majorTickStep(float pixelsPerTick) {
    constexpr int steps[] = {20, 40, 100, 200, 400, 600, 1200, 2400, 6000, 12000};
    for (int step : steps)
        if (step * pixelsPerTick >= 60.0f) return step;
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

void TimelinePanel::setViewPreferences(float trackListWidthRatio, float pixelsPerTick, float horizontalScroll) {
    mTrackListWidthRatio = std::clamp(trackListWidthRatio, 0.18f, 0.55f);
    mPixelsPerTick       = std::clamp(pixelsPerTick, 0.05f, 5.0f);
    mScrollX             = std::max(0.0f, horizontalScroll);
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
    mTrackTree.setMarkerExpanded(mMarkersExpanded);
    mTrackTree.rebuild(*project);
    int displayTick = mPendingSeekTick >= 0 ? mPendingSeekTick : state.currentTick;
    if (mPendingSeekTick >= 0 && state.currentTick == mPendingSeekTick) mPendingSeekTick = -1;

    ImVec2 const fullMin   = ImGui::GetCursorScreenPos();
    ImVec2 const available = ImGui::GetContentRegionAvail();
    if (available.x < 220.0f || available.y < 120.0f) return;
    ImVec2 const fullMax{fullMin.x + available.x, fullMin.y + available.y};
    auto*        drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(fullMin, fullMax, IM_COL32(18, 19, 23, 255));

    ImGui::SetCursorScreenPos(fullMin);
    ImGui::BeginChild("##TimelineToolbar", {available.x, kToolbarHeight}, false, ImGuiWindowFlags_NoScrollbar);
    ImGui::Text("%s / %s", formatTick(displayTick).c_str(), formatTick(state.totalTicks).c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.canUndo);
    if (ImGui::Button("playback.refactorEditor.menu.undo"_tr().c_str(), {42, 28}))
        submitEdit({EditorActionType::UndoEditorEdit});
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.canRedo);
    if (ImGui::Button("playback.refactorEditor.menu.redo"_tr().c_str(), {42, 28}))
        submitEdit({EditorActionType::RedoEditorEdit});
    ImGui::EndDisabled();
    ImGui::SameLine();
    auto const* selectedCamera = editor.selection().getAs<editing::model::SelectedCamera>();
    ImGui::BeginDisabled(selectedCamera == nullptr);
    if (ImGui::Button("playback.refactorEditor.timeline.addKey"_tr().c_str(), {64, 28})) {
        EditorAction action{EditorActionType::AddCameraKeyframe};
        action.id   = selectedCamera->cameraId;
        action.tick = state.currentTick;
        submitEdit(std::move(action));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("playback.refactorEditor.timeline.addCamera"_tr().c_str(), {74, 28})) {
        EditorAction action{EditorActionType::AddFreeCamera};
        action.name = "playback.refactorEditor.defaults.camera"_tr(project->cameras.size() + 1);
        submitEdit(std::move(action));
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("playback.refactorEditor.timeline.snap"_tr().c_str());
    ImGui::SameLine();
    ImGui::Checkbox("##timeline-snap", &mSnapEnabled);
    ImGui::SameLine();
    if (ImGui::Button("-", {28, 28})) mPixelsPerTick = std::max(0.05f, mPixelsPerTick / 1.15f);
    ImGui::SameLine();
    float percent = mPixelsPerTick / 0.25f * 100.0f;
    ImGui::SetNextItemWidth(64.0f);
    if (ImGui::DragFloat("##timeline-zoom", &percent, 1.0f, 20.0f, 2000.0f, "%.0f%%"))
        mPixelsPerTick = std::clamp(percent * 0.25f / 100.0f, 0.05f, 5.0f);
    ImGui::SameLine();
    if (ImGui::Button("+", {28, 28})) mPixelsPerTick = std::min(5.0f, mPixelsPerTick * 1.15f);
    ImGui::EndChild();

    float const workTop      = fullMin.y + kToolbarHeight;
    float const workBottom   = fullMax.y - kTransportHeight;
    float const listWidth    = std::clamp(available.x * mTrackListWidthRatio, 160.0f, available.x - 180.0f);
    float const canvasLeft   = fullMin.x + listWidth + kSplitterThickness;
    float const canvasWidth  = fullMax.x - canvasLeft;
    float const bodyTop      = workTop + kRulerHeight;
    float const bodyBottom   = workBottom;
    float const contentWidth = std::max(canvasWidth, state.totalTicks * mPixelsPerTick);
    float const maxScroll    = std::max(0.0f, contentWidth - canvasWidth);
    mScrollX                 = std::clamp(mScrollX, 0.0f, maxScroll);

    ImGui::SetCursorScreenPos({fullMin.x + listWidth - kSplitterThickness * 0.5f, workTop});
    ImGui::InvisibleButton("##timeline-list-splitter", {kSplitterThickness, workBottom - workTop});
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        mTrackListWidthRatio = std::clamp((ImGui::GetMousePos().x - fullMin.x) / available.x, 0.18f, 0.55f);
    }
    drawList->AddLine(
        {fullMin.x + listWidth, workTop},
        {fullMin.x + listWidth, workBottom},
        IM_COL32(80, 82, 92, 255),
        2.0f
    );

    ImGui::SetCursorScreenPos({fullMin.x, workTop});
    ImGui::BeginChild("##TimelineTrackList", {listWidth, workBottom - workTop}, false, ImGuiWindowFlags_NoScrollbar);
    char search[128]{};
    std::snprintf(search, sizeof(search), "%s", mTrackSearch.c_str());
    ImGui::SetNextItemWidth(listWidth - 8.0f);
    if (ImGui::InputTextWithHint(
            "##timeline-search",
            "playback.refactorEditor.timeline.searchCameras"_tr().c_str(),
            search,
            sizeof(search)
        ))
        mTrackSearch = search;
    for (auto const& row : mTrackTree.rows()) {
        bool selected = (row.kind == editing::model::TrackRowKind::Sequence
                         && editor.selection().getAs<editing::model::SelectedSequence>())
                     || (row.kind == editing::model::TrackRowKind::WorldActor
                         && editor.selection().getAs<editing::model::SelectedWorldActor>())
                     || (row.kind == editing::model::TrackRowKind::Camera
                         && editor.selection().getAs<editing::model::SelectedCamera>()
                         && editor.selection().getAs<editing::model::SelectedCamera>()->cameraId == row.id.substr(7));
        if (row.kind == editing::model::TrackRowKind::Camera) {
            ImGui::PushID(row.id.c_str());
            if (ImGui::Selectable((std::string("C  ") + row.name).c_str(), selected))
                editor.selection().select(editing::model::SelectedCamera{row.id.substr(7)});
            ImGui::PopID();
        } else if (row.kind == editing::model::TrackRowKind::Sequence) {
            std::string const label = "S  " + "playback.refactorEditor.details.cameraSequence"_tr();
            if (ImGui::Selectable(label.c_str(), selected))
                editor.selection().select(editing::model::SelectedSequence{});
        } else if (row.kind == editing::model::TrackRowKind::WorldActor) {
            std::string const label = "W  " + "playback.refactorEditor.details.worldActor"_tr();
            if (ImGui::Selectable(label.c_str(), selected))
                editor.selection().select(editing::model::SelectedWorldActor{});
        } else {
            std::string const label = "M  " + "playback.refactorEditor.timeline.markers"_tr();
            if (ImGui::Selectable(label.c_str(), false)) editor.selection().clear();
        }
        if (row.locked) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", "playback.refactorEditor.timeline.lock"_tr().c_str());
        }
    }
    ImGui::EndChild();

    drawList->AddRectFilled({canvasLeft, workTop}, {fullMax.x, workBottom}, IM_COL32(24, 25, 30, 255));
    drawList->PushClipRect({canvasLeft, workTop}, {fullMax.x, workBottom}, true);
    int const majorStep = majorTickStep(mPixelsPerTick);
    int const minorStep = std::max(1, majorStep / 5);
    int const firstTick = std::max(0, static_cast<int>(std::floor(mScrollX / mPixelsPerTick / minorStep)) * minorStep);
    for (int tick = firstTick; tick <= state.totalTicks; tick += minorStep) {
        float x = canvasLeft + tick * mPixelsPerTick - mScrollX;
        if (x < canvasLeft || x > fullMax.x) continue;
        bool const major = tick % majorStep == 0;
        drawList->AddLine(
            {x, workTop + (major ? 14.0f : 21.0f)},
            {x, workTop + kRulerHeight},
            major ? IM_COL32(155, 158, 168, 255) : IM_COL32(72, 75, 84, 255)
        );
        if (major)
            drawList->AddText({x + 3.0f, workTop + 2.0f}, IM_COL32(190, 193, 202, 255), formatTick(tick).c_str());
    }

    ImGui::SetCursorScreenPos({canvasLeft, workTop});
    ImGui::InvisibleButton("##timeline-ruler", {canvasWidth, kRulerHeight});
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        displayTick = std::clamp(
            static_cast<int>((ImGui::GetMousePos().x - canvasLeft + mScrollX) / mPixelsPerTick),
            0,
            state.totalTicks
        );
    }
    if (ImGui::IsItemDeactivated()) submitSeek(displayTick);

    auto segmentLabel = [&project](editing::model::SequenceSegment const& segment) -> std::string {
        if (segment.cameraId.empty())
            return project->cameras.empty() ? "playback.refactorEditor.timeline.noCamera"_tr()
                                            : "playback.refactorEditor.timeline.autoFirstCamera"_tr();
        auto it = std::find_if(project->cameras.begin(), project->cameras.end(), [&segment](auto const& camera) {
            return camera.id == segment.cameraId;
        });
        return it == project->cameras.end() ? "playback.refactorEditor.timeline.missingCamera"_tr() : it->name;
    };
    auto tickFromMouse = [&] {
        return std::clamp(
            static_cast<int>((ImGui::GetMousePos().x - canvasLeft + mScrollX) / mPixelsPerTick),
            0,
            state.totalTicks
        );
    };
    float y = bodyTop + 2.0f;
    for (auto const& row : mTrackTree.rows()) {
        float const rowBottom = y + row.height;
        drawList->AddRectFilled({canvasLeft, y}, {fullMax.x, rowBottom}, IM_COL32(31, 32, 38, 255));
        if (row.kind == editing::model::TrackRowKind::Sequence) {
            for (auto const& segment : project->sequence) {
                ImVec2 minimum{canvasLeft + segment.startTick * mPixelsPerTick - mScrollX, y + 4.0f};
                ImVec2 maximum{canvasLeft + segment.endTick * mPixelsPerTick - mScrollX, rowBottom - 4.0f};
                bool   selected =
                    editor.selection().getAs<editing::model::SelectedSequenceSegment>()
                    && editor.selection().getAs<editing::model::SelectedSequenceSegment>()->segmentId == segment.id;
                drawList->AddRectFilled(minimum, maximum, color(segment.color), 3.0f);
                drawList->AddRect(
                    minimum,
                    maximum,
                    selected ? IM_COL32(240, 192, 32, 255) : IM_COL32(100, 160, 225, 255),
                    3.0f
                );
                std::string const label = segmentLabel(segment);
                drawList->AddText(
                    {minimum.x + 5.0f, minimum.y + 6.0f},
                    IM_COL32(245, 245, 247, 255),
                    label.c_str()
                );
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && contains(minimum, maximum, ImGui::GetMousePos())) {
                    editor.selection().select(editing::model::SelectedSequenceSegment{segment.id});
                    if (!segment.locked
                        && (std::abs(ImGui::GetMousePos().x - minimum.x) < 8.0f
                            || std::abs(ImGui::GetMousePos().x - maximum.x) < 8.0f)) {
                        mDraggingSegmentId  = segment.id;
                        mDraggingWorldActor = false;
                        mDraggingStart =
                            std::abs(ImGui::GetMousePos().x - minimum.x) < std::abs(ImGui::GetMousePos().x - maximum.x);
                        mDragStartTick = segment.startTick;
                        mDragEndTick   = segment.endTick;
                    }
                }
            }
        } else if (row.kind == editing::model::TrackRowKind::WorldActor) {
            for (auto const& segment : project->worldActor.segments) {
                ImVec2 minimum{canvasLeft + segment.startTick * mPixelsPerTick - mScrollX, y + 4.0f};
                ImVec2 maximum{canvasLeft + segment.endTick * mPixelsPerTick - mScrollX, rowBottom - 4.0f};
                bool   selected =
                    editor.selection().getAs<editing::model::SelectedWorldActorSegment>()
                    && editor.selection().getAs<editing::model::SelectedWorldActorSegment>()->segmentId == segment.id;
                drawList->AddRectFilled(minimum, maximum, color(segment.color), 3.0f);
                drawList->AddRect(
                    minimum,
                    maximum,
                    selected ? IM_COL32(240, 192, 32, 255) : IM_COL32(225, 135, 70, 255),
                    3.0f
                );
                char label[32]{};
                std::snprintf(label, sizeof(label), "%.2fx", segment.speed);
                drawList->AddText({minimum.x + 5.0f, minimum.y + 6.0f}, IM_COL32(245, 245, 247, 255), label);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && contains(minimum, maximum, ImGui::GetMousePos())) {
                    editor.selection().select(editing::model::SelectedWorldActorSegment{segment.id});
                    if (!segment.locked
                        && (std::abs(ImGui::GetMousePos().x - minimum.x) < 8.0f
                            || std::abs(ImGui::GetMousePos().x - maximum.x) < 8.0f)) {
                        mDraggingSegmentId  = segment.id;
                        mDraggingWorldActor = true;
                        mDraggingStart =
                            std::abs(ImGui::GetMousePos().x - minimum.x) < std::abs(ImGui::GetMousePos().x - maximum.x);
                        mDragStartTick = segment.startTick;
                        mDragEndTick   = segment.endTick;
                    }
                }
            }
        } else if (row.kind == editing::model::TrackRowKind::Camera && row.cameraIndex >= 0
                   && row.cameraIndex < static_cast<int>(project->cameras.size())) {
            auto const& camera = project->cameras[row.cameraIndex];
            for (auto const& key : camera.keys) {
                float x = canvasLeft + key.tick * mPixelsPerTick - mScrollX;
                drawList->AddCircleFilled({x, (y + rowBottom) * 0.5f}, 5.0f, IM_COL32(128, 192, 240, 255));
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && std::abs(ImGui::GetMousePos().x - x) <= 7.0f
                    && ImGui::GetMousePos().y >= y && ImGui::GetMousePos().y <= rowBottom)
                    editor.selection().select(editing::model::SelectedKeyframe{camera.id, key.id});
            }
        } else if (row.kind == editing::model::TrackRowKind::Marker) {
            for (auto const& marker : project->markers) {
                float x = canvasLeft + marker.tick * mPixelsPerTick - mScrollX;
                drawList->AddLine({x, y}, {x, rowBottom}, IM_COL32(240, 192, 32, 255));
                drawList->AddText({x + 4.0f, y + 2.0f}, IM_COL32(240, 210, 100, 255), marker.label.c_str());
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && std::abs(ImGui::GetMousePos().x - x) <= 6.0f
                    && ImGui::GetMousePos().y >= y && ImGui::GetMousePos().y <= rowBottom)
                    editor.selection().select(editing::model::SelectedMarker{marker.id});
            }
        }
        y = rowBottom + 2.0f;
    }

    if (!mDraggingSegmentId.empty()) {
        int tick = tickFromMouse();
        if (mSnapEnabled) tick = std::clamp(static_cast<int>(std::round(tick / 20.0f)) * 20, 0, state.totalTicks);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float x = canvasLeft + tick * mPixelsPerTick - mScrollX;
            drawList->AddLine({x, bodyTop}, {x, bodyBottom}, IM_COL32(240, 192, 32, 180), 2.0f);
        } else {
            EditorAction action{
                mDraggingWorldActor ? EditorActionType::TrimWorldActor : EditorActionType::TrimSequence
            };
            action.id   = mDraggingSegmentId;
            action.tick = mDraggingStart ? tick : mDragStartTick;
            action.kind = mDraggingStart ? mDragEndTick : tick;
            submitEdit(std::move(action));
            mDraggingSegmentId.clear();
        }
    }

    float const playheadX = std::clamp(canvasLeft + displayTick * mPixelsPerTick - mScrollX, canvasLeft, fullMax.x);
    drawList->AddLine({playheadX, workTop}, {playheadX, bodyBottom}, IM_COL32(240, 192, 32, 255), 2.0f);
    drawList->PopClipRect();
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
        float const wheel = ImGui::GetIO().MouseWheel;
        if (ImGui::GetIO().KeyShift) {
            float old      = mPixelsPerTick;
            mPixelsPerTick = std::clamp(mPixelsPerTick * (wheel > 0.0f ? 1.15f : 1.0f / 1.15f), 0.05f, 5.0f);
            mScrollX       = std::max(0.0f, mScrollX + displayTick * (mPixelsPerTick - old));
        } else {
            mScrollX = std::clamp(mScrollX - wheel * 60.0f, 0.0f, maxScroll);
        }
    }

    ImGui::SetCursorScreenPos({canvasLeft, workBottom - 18.0f});
    if (maxScroll > 0.0f) {
        ImGui::SetNextItemWidth(canvasWidth);
        ImGui::SliderFloat("##timeline-scroll", &mScrollX, 0.0f, maxScroll, "", ImGuiSliderFlags_NoInput);
    }
    ImGui::SetCursorScreenPos({fullMin.x, workBottom});
    ImGui::BeginChild("##TimelineTransport", {available.x, kTransportHeight}, false, ImGuiWindowFlags_NoScrollbar);
    if (ImGui::Button("|<", {32, 28})) submitEdit({EditorActionType::SkipToStart});
    ImGui::SameLine();
    if (ImGui::Button("<<", {32, 28})) {
        EditorAction action{EditorActionType::Seek};
        action.tick = std::max(0, state.currentTick - 20);
        submitEdit(std::move(action));
    }
    ImGui::SameLine();
    std::string const playPause = state.paused ? "playback.refactorEditor.timeline.play"_tr()
                                               : "playback.refactorEditor.timeline.pause"_tr();
    if (ImGui::Button(playPause.c_str(), {52, 28})) submitEdit({EditorActionType::TogglePause});
    ImGui::SameLine();
    if (ImGui::Button(">>", {32, 28})) {
        EditorAction action{EditorActionType::Seek};
        action.tick = std::min(state.totalTicks, state.currentTick + 20);
        submitEdit(std::move(action));
    }
    ImGui::SameLine();
    if (ImGui::Button(">|", {32, 28})) submitEdit({EditorActionType::SkipToEnd});
    ImGui::SameLine();
    ImGui::BeginDisabled();
    ImGui::Button("playback.refactorEditor.timeline.loop"_tr().c_str(), {48, 28});
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%.2fx", state.playbackSpeed);
    ImGui::EndChild();
}

} // namespace playback::editor::ui
