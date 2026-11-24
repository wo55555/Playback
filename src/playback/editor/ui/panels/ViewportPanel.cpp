#include "ViewportPanel.h"

#include "playback/editor/input/KeyMap.h"
#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/PlayerPicker.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/exporting/ExportTypes.h"
#include "playback/state/editing/TimelineNavigation.h"

#include "imgui.h"
#include "ll/api/i18n/I18n.h"

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <utility>

namespace playback::editor::ui {
using namespace playback::state;

using namespace ll::i18n_literals;

namespace {

// Pixels around an entity's projected box that still count as clicking it.
constexpr float kPickSlackPixels = 6.0f;
// Entries in the nearby entities menu.
constexpr size_t kNearbyEntityLimit = 30;

void submitFollow(PanelContext const& ctx, std::string choice) {
    EditorAction action{EditorActionType::SetFollowRecordedPlayer};
    action.value = true;
    action.id    = std::move(choice);
    ctx.submitAction(std::move(action));
}

std::string entityMenuLabel(replay::ReplaySession::ReplayEntityInfo const& entity) {
    if (entity.recorder) return "playback.refactorEditor.playerPicker.recorder"_tr() + " (" + entity.label + ")";
    return entity.label;
}

struct SubjectSelectionInfo {
    std::vector<editing::model::KeyframeRef> refs;
    bool                                     anyUnlocked{};
    bool                                     allSameSubject{true};
};

SubjectSelectionInfo collectSelectedPersonKeyframes(PanelContext const& ctx, std::string_view choice) {
    SubjectSelectionInfo info;
    auto const           project = ctx.state.project;
    if (!project) return info;
    for (auto const& entry : ctx.selection.keyframes()) {
        auto const it =
            std::ranges::find_if(project->cameras, [&](auto const& camera) { return camera.id == entry.trackId; });
        if (it == project->cameras.end()) continue;
        auto const key = it->keysByTick.find(entry.tick);
        if (key == it->keysByTick.end()) continue;
        if (key->second.mode == editing::model::CameraKeyframeMode::Interpolated) continue;
        info.refs.push_back({entry.trackId, entry.tick, std::nullopt});
        info.anyUnlocked    |= !it->locked;
        info.allSameSubject &= (key->second.subject == choice);
    }
    return info;
}

struct LookAtSelectionInfo {
    std::vector<editing::model::KeyframeRef> refs;
    bool                                     anyUnlocked{};
    bool                                     allSameTarget{true};
    float                                    firstOffsetY{0.0f};
};

LookAtSelectionInfo collectSelectedFreeKeyframes(PanelContext const& ctx, std::string_view choice) {
    LookAtSelectionInfo info;
    auto const          project = ctx.state.project;
    if (!project) return info;
    for (auto const& entry : ctx.selection.keyframes()) {
        auto const it =
            std::ranges::find_if(project->cameras, [&](auto const& camera) { return camera.id == entry.trackId; });
        if (it == project->cameras.end()) continue;
        auto const key = it->keysByTick.find(entry.tick);
        if (key == it->keysByTick.end()) continue;
        if (key->second.mode != editing::model::CameraKeyframeMode::Interpolated) continue;
        if (info.refs.empty()) info.firstOffsetY = key->second.lookAtOffsetY;
        info.refs.push_back({entry.trackId, entry.tick, std::nullopt});
        info.anyUnlocked   |= !it->locked;
        info.allSameTarget &= (key->second.lookAtEnabled && key->second.lookAtTarget == choice);
    }
    return info;
}

} // namespace

std::optional<replay::ReplaySession::ReplayEntityInfo>
ViewportPanel::pickEntityAt(PanelContext const& ctx, ImVec2 const& mouse) const {
    if (!ctx.cameraProjection) return std::nullopt;
    auto const&  matrix = ctx.cameraProjection->viewProjection;
    double const nearW  = std::max(0.001, static_cast<double>(ctx.cameraProjection->nearClipW));
    auto const   project = [&](double x, double y, double z) -> std::optional<std::pair<ImVec2, double>> {
        ::glm::dvec4 const clip = matrix * ::glm::dvec4{x, y, z, 1.0};
        if (!std::isfinite(clip.w) || clip.w < nearW) return std::nullopt;
        double const ndcX = clip.x / clip.w;
        double const ndcY = clip.y / clip.w;
        if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) return std::nullopt;
        return std::pair{
            ImVec2{
                mVideoRect.min.x + static_cast<float>(ndcX * 0.5 + 0.5) * mVideoRect.GetWidth(),
                mVideoRect.min.y + static_cast<float>(0.5 - ndcY * 0.5) * mVideoRect.GetHeight(),
            },
            clip.w,
        };
    };

    std::optional<replay::ReplaySession::ReplayEntityInfo> best;
    double                                                 bestDepth = std::numeric_limits<double>::max();
    for (auto const& entity : replay::ReplaySession::getInstance().replayEntities()) {
        // A screen rectangle from the feet and head points, widened to the box half width at that depth.
        auto const feet = project(entity.x, entity.y, entity.z);
        auto const head = project(entity.x, entity.y + entity.height, entity.z);
        if (!feet || !head) continue;
        float const top    = std::min(feet->first.y, head->first.y);
        float const bottom = std::max(feet->first.y, head->first.y);
        float const halfW  = std::max(4.0f, (bottom - top) * 0.3f) + kPickSlackPixels;
        float const centerX = (feet->first.x + head->first.x) * 0.5f;
        if (mouse.x < centerX - halfW || mouse.x > centerX + halfW) continue;
        if (mouse.y < top - kPickSlackPixels || mouse.y > bottom + kPickSlackPixels) continue;
        double const depth = std::min(feet->second, head->second);
        if (depth >= bestDepth) continue;
        bestDepth = depth;
        best      = entity;
    }
    return best;
}

void ViewportPanel::drawEntityPickMenu(PanelContext const& ctx) {
    float const scale = metrics::scale();
    ImVec4      bg    = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
    bg.w *= 0.86f;
    ImGui::PushStyleColor(ImGuiCol_PopupBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14.0f * scale, 10.0f * scale});
    bool const open = ImGui::BeginPopup("##viewport-entity-menu");
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (!open) return;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {10.0f * scale, 8.0f * scale});
    auto&      session   = replay::ReplaySession::getInstance();
    bool const following = ctx.state.followingRecordedPlayer;
    if (mPickedEntity) {
        auto const& entity   = *mPickedEntity;
        bool const  selected = following && session.userReplaySubjectChoice() == entity.choice;
        std::string const label =
            "playback.refactorEditor.viewport.followEntity"_tr(entityMenuLabel(entity)) + "##pick-follow";
        if (ImGui::MenuItem(label.c_str(), nullptr, selected)) submitFollow(ctx, entity.choice);
        auto target = collectSelectedPersonKeyframes(ctx, entity.choice);
        if (!target.refs.empty()) {
            std::string const setLabel =
                (target.refs.size() == 1
                     ? "playback.refactorEditor.viewport.setKeyframeSubject"_tr(entityMenuLabel(entity))
                     : "playback.refactorEditor.viewport.setKeyframesSubject"_tr(
                           target.refs.size(),
                           entityMenuLabel(entity)
                       ))
                + "##pick-keyframe-subject";
            if (ImGui::MenuItem(setLabel.c_str(), nullptr, target.allSameSubject, target.anyUnlocked)) {
                if (target.refs.size() == 1) {
                    EditorAction action{EditorActionType::SetKeyframeSubject};
                    action.id          = target.refs[0].cameraId;
                    action.tick        = target.refs[0].tick;
                    action.secondaryId = entity.choice;
                    ctx.submitAction(std::move(action));
                } else {
                    EditorAction action{EditorActionType::SetKeyframesSubject};
                    action.keyframeRefs = std::move(target.refs);
                    action.secondaryId  = entity.choice;
                    ctx.submitAction(std::move(action));
                }
            }
        }
        auto lookAtTarget = collectSelectedFreeKeyframes(ctx, entity.choice);
        if (!lookAtTarget.refs.empty()) {
            std::string const lookAtLabel =
                (lookAtTarget.refs.size() == 1
                     ? "playback.refactorEditor.viewport.lookAtEntity"_tr(entityMenuLabel(entity))
                     : "playback.refactorEditor.viewport.lookAtEntityKeyframes"_tr(
                           lookAtTarget.refs.size(),
                           entityMenuLabel(entity)
                       ))
                + "##pick-keyframe-lookat";
            if (ImGui::MenuItem(lookAtLabel.c_str(), nullptr, lookAtTarget.allSameTarget, lookAtTarget.anyUnlocked)) {
                if (lookAtTarget.refs.size() == 1) {
                    EditorAction action{EditorActionType::SetKeyframeLookAt};
                    action.id          = lookAtTarget.refs[0].cameraId;
                    action.tick        = lookAtTarget.refs[0].tick;
                    action.value       = true;
                    action.secondaryId = entity.choice;
                    action.speed       = lookAtTarget.firstOffsetY;
                    ctx.submitAction(std::move(action));
                } else {
                    EditorAction action{EditorActionType::SetKeyframesLookAt};
                    action.keyframeRefs  = std::move(lookAtTarget.refs);
                    action.value         = true;
                    action.secondaryId   = entity.choice;
                    action.secondaryTick = 1;
                    ctx.submitAction(std::move(action));
                }
            }
        }
    } else {
        ImGui::TextDisabled("%s", "playback.refactorEditor.viewport.noEntityHere"_tr().c_str());
    }
    ImGui::Separator();
    if (ImGui::MenuItem("playback.refactorEditor.viewport.freeCamera"_tr().c_str(), nullptr, !following)) {
        ctx.selection.clear();
        ctx.submitAction({EditorActionType::ClearPreviewCamera});
    }
    ImGui::PopStyleVar();
    ImGui::EndPopup();
}

void ViewportPanel::draw(PanelContext const& ctx, bool maximized) {
    drawToolbar(ctx);

    ImVec2 const sceneSize = ImGui::GetContentRegionAvail();
    ImDrawList*  drawList  = ImGui::GetWindowDrawList();
    ImVec2 const sceneMin  = ImGui::GetCursorScreenPos();
    ImVec2 const sceneMax{sceneMin.x + sceneSize.x, sceneMin.y + sceneSize.y};
    drawList->AddRectFilled(sceneMin, sceneMax, theme::kViewportBg);

    float const  sceneAspectRatio = sceneSize.x / std::max(1.0f, sceneSize.y);
    ImVec2 const videoSize        = sceneAspectRatio > mVideoAspectRatio
                                      ? ImVec2(sceneSize.y * mVideoAspectRatio, sceneSize.y)
                                      : ImVec2(sceneSize.x, sceneSize.x / mVideoAspectRatio);
    ImVec2 const videoMin(
        sceneMin.x + (sceneSize.x - videoSize.x) * 0.5f,
        sceneMin.y + (sceneSize.y - videoSize.y) * 0.5f
    );
    ImVec2 const videoMax(videoMin.x + videoSize.x, videoMin.y + videoSize.y);
    mVideoRect = {videoMin, videoMax};

    if (mGameTexture) {
        drawList->AddImage(ImTextureRef(mGameTexture), videoMin, videoMax);
        mCameraPath.draw(ctx, mVideoRect, drawList);
    } else {
        mCameraPath.clear();
    }
    // Neutral 1px frame: the accent blue is reserved for selection state elsewhere in the editor.
    drawList->AddRect(videoMin, videoMax, theme::kBorder);
    ImGui::SetCursorScreenPos(videoMin);
    // The floating transport overlaps this hit box; without this it would keep the hover id.
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##viewport-video", videoSize);
    bool const thirdPersonOrbitActive = ctx.state.editorVisible && ctx.state.followingRecordedPlayer
                                     && ctx.state.followThirdPerson
                                     && !exporting::isExportActive(ctx.state.exportStatus.state);
    if (thirdPersonOrbitActive) {
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ImVec2 const delta = ImGui::GetIO().MouseDelta;
            if (delta.x != 0.0f || delta.y != 0.0f) {
                constexpr float kOrbitDegreesPerPixel = 0.35f;
                ctx.commands
                    .adjustThirdPersonOrbit(delta.x * kOrbitDegreesPerPixel, delta.y * kOrbitDegreesPerPixel, 0.0f);
            }
        }
        if (ImGui::IsItemHovered()) {
            float const wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                ctx.commands.adjustThirdPersonOrbit(0.0f, 0.0f, -wheel * 0.5f);
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
                ctx.commands.resetThirdPersonOrbit();
            }
        }
    }
    // Left click hands the mouse to the game (or orbits in 3P follow mode); right click picks whatever entity is under
    // the cursor.
    if (ctx.state.replayVisible && mGameTexture && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        mPickedEntity = pickEntityAt(ctx, ImGui::GetMousePos());
        ImGui::OpenPopup("##viewport-entity-menu");
    }
    drawEntityPickMenu(ctx);

    mOverlayRect = {};
    if (mInfoOverlayVisible) drawInfoOverlay(ctx, videoMin, drawList);
    if (maximized) drawFloatingTransport(ctx, sceneMin, sceneMax);
}

void ViewportPanel::drawToolbar(PanelContext const& ctx) {
    auto const&  state    = ctx.state;
    auto const   project  = state.project;
    float const  height   = metrics::toolbarRow();
    ImVec2 const origin   = ImGui::GetCursorScreenPos();
    float const  width    = ImGui::GetContentRegionAvail().x;
    ImDrawList*  drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, theme::kBgHeader);
    drawList->AddLine({origin.x, origin.y + height}, {origin.x + width, origin.y + height}, theme::kBorder);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild("##ViewportToolbar", {width, height}, false, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos({metrics::gutter(), (height - metrics::iconButton()) * 0.5f});

    auto const* selectedCamera = ctx.selection.getAs<state::editing::model::SelectedCamera>();
    bool const  following        = state.followingRecordedPlayer;
    bool        openPlayerPicker = false;
    ImVec2      playerAnchorMin{};
    ImVec2      playerAnchorMax{};
    // Follow mode owns the viewport when active; otherwise the track at the playhead (or selected/free camera) does.
    state::editing::model::CameraEntity const* activeCamera = nullptr;
    if (!following && project && !state.activeCameraId.empty()) {
        auto const camera =
            std::ranges::find(project->cameras, state.activeCameraId, &state::editing::model::CameraEntity::id);
        if (camera != project->cameras.end()) activeCamera = &*camera;
    }
    auto const modeIcon = [](state::editing::model::CameraKeyframeMode mode) -> char const* {
        using Mode = state::editing::model::CameraKeyframeMode;
        switch (mode) {
        case Mode::FirstPerson:
            return ICON_EYE;
        case Mode::ThirdPerson:
            return ICON_USER;
        case Mode::Interpolated:
        default:
            return ICON_CAMERA;
        }
    };
    std::string cameraLabel = "playback.refactorEditor.viewport.freeCamera"_tr();
    char const* cameraIcon  = ICON_CAMERA;
    if (activeCamera) {
        cameraLabel = activeCamera->name;
        auto const activeMode =
            state.cameraTimeline
                ? state.cameraTimeline->shotAt(state.currentTick).mode
                : state::editing::model::CameraKeyframeMode::Interpolated;
        cameraIcon = modeIcon(activeMode);
    } else if (following) {
        auto const subject = playerSubjectLabel(state.replayPlayers, state.replayEntities, state.replaySubjectChoice);
        cameraLabel        = state.followThirdPerson ? "playback.refactorEditor.viewport.followSubjectThird"_tr(subject)
                                                     : "playback.refactorEditor.viewport.followSubject"_tr(subject);
        cameraIcon         = state.followThirdPerson ? ICON_USER : ICON_EYE;
    } else if (project && selectedCamera) {
        auto const camera =
            std::ranges::find(project->cameras, selectedCamera->cameraId, &state::editing::model::CameraEntity::id);
        if (camera != project->cameras.end()) {
            cameraLabel = camera->name;
            if (!camera->keysByTick.empty()) {
                auto const upper = camera->keysByTick.upper_bound(state.currentTick);
                if (upper != camera->keysByTick.begin()) cameraIcon = modeIcon(std::prev(upper)->second.mode);
            }
        }
    }
    if (widgets::dropdownChip(
            "viewport-camera",
            cameraLabel.c_str(),
            "playback.refactorEditor.viewport.cameraTooltip"_tr().c_str(),
            project != nullptr,
            cameraIcon
        )) {
        widgets::openAnchoredPopup("##viewport-camera-menu");
    }
    playerAnchorMin = ImGui::GetItemRectMin();
    playerAnchorMax = ImGui::GetItemRectMax();
    if (widgets::beginAnchoredPopup("##viewport-camera-menu")) {
        if (ImGui::MenuItem(
                "playback.refactorEditor.viewport.freeCamera"_tr().c_str(),
                nullptr,
                !selectedCamera && !following
            )) {
            ctx.selection.clear();
            ctx.submitAction({EditorActionType::ClearPreviewCamera});
        }
        if (project && !project->cameras.empty()) ImGui::Separator();
        if (project) {
            for (auto const& camera : project->cameras) {
                bool const        current = !following && selectedCamera && selectedCamera->cameraId == camera.id;
                std::string const label   = camera.name + "##cam-" + camera.id;
                if (ImGui::MenuItem(label.c_str(), nullptr, current)) {
                    ctx.selection.select(state::editing::model::SelectedCamera{camera.id});
                    ctx.commands.showSelectionInspector();
                    EditorAction action{EditorActionType::SetPreviewCamera};
                    action.id = camera.id;
                    ctx.submitAction(std::move(action));
                }
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(
                "playback.refactorEditor.viewport.teleportToPlayer"_tr().c_str(),
                nullptr,
                false,
                state.replayVisible
            )) {
            ctx.submitAction({EditorActionType::TeleportToRecordedPlayer});
        }
        if (ImGui::MenuItem(
                "playback.refactorEditor.viewport.followTarget"_tr().c_str(),
                nullptr,
                following,
                state.replayVisible
            )) {
            ImGui::CloseCurrentPopup();
            openPlayerPicker = true;
        }
        std::string const perspectiveShortcut =
            input::KeyMap::displayString(input::EditorKeybind::ToggleFollowPerspective);
        if (ImGui::MenuItem(
                "playback.refactorEditor.viewport.firstPerson"_tr().c_str(),
                perspectiveShortcut.c_str(),
                following && !state.followThirdPerson,
                state.replayVisible
            )) {
            if (!following) submitFollow(ctx, state.replaySubjectChoice);
            EditorAction action{EditorActionType::SetFollowThirdPerson};
            action.value = false;
            ctx.submitAction(std::move(action));
        }
        if (ImGui::MenuItem(
                "playback.refactorEditor.viewport.thirdPerson"_tr().c_str(),
                perspectiveShortcut.c_str(),
                following && state.followThirdPerson,
                state.replayVisible
            )) {
            if (!following) submitFollow(ctx, state.replaySubjectChoice);
            EditorAction action{EditorActionType::SetFollowThirdPerson};
            action.value = true;
            ctx.submitAction(std::move(action));
        }
        // Quick access to nearby entities, prioritizing living mobs; players and full search live in the picker above.
        if (ImGui::BeginMenu(
                "playback.refactorEditor.viewport.nearbyEntities"_tr().c_str(),
                state.replayVisible && !state.replayEntities.empty()
            )) {
            size_t     shown          = 0;
            auto const emitEntityItem = [&](state::ReplayEntityView const& entity) {
                if (shown >= kNearbyEntityLimit) return;
                ++shown;
                bool const        selected = following && state.replaySubjectChoice == entity.choice;
                std::string const label    = entity.label + "##" + entity.choice;
                if (ImGui::MenuItem(label.c_str(), nullptr, selected)) {
                    EditorAction action{EditorActionType::SetFollowRecordedPlayer};
                    action.value = !selected;
                    action.id    = entity.choice;
                    ctx.submitAction(std::move(action));
                }
            };
            for (auto const& entity : state.replayEntities) {
                if (entity.mob) emitEntityItem(entity);
            }
            for (auto const& entity : state.replayEntities) {
                if (!entity.mob) emitEntityItem(entity);
            }
            ImGui::EndMenu();
        }
        widgets::endAnchoredPopup();
    }
    if (openPlayerPicker) widgets::openAnchoredPopup("##viewport-player-picker", playerAnchorMin, playerAnchorMax);
    PlayerPickerResult playerPick;
    if (drawPlayerPicker(
            "##viewport-player-picker",
            PlayerPickerMode::Follow,
            state.replayPlayers,
            state.replayEntities,
            state.replaySubjectChoice,
            following,
            playerPick
        )) {
        EditorAction action{EditorActionType::SetFollowRecordedPlayer};
        if (playerPick.stop) action.value = false;
        else {
            action.value = true;
            action.id    = std::move(playerPick.uuid);
        }
        ctx.submitAction(std::move(action));
    }

    ImGui::SameLine(0.0f, metrics::gutter());
    char aspectText[32]{};
    if (char const* label = widgets::aspectPresetLabel(mVideoAspectRatio))
        std::snprintf(aspectText, sizeof(aspectText), "%s", label);
    else std::snprintf(aspectText, sizeof(aspectText), "%.2f:1", mVideoAspectRatio);
    if (widgets::dropdownChip(
            "viewport-aspect",
            aspectText,
            "playback.refactorEditor.viewport.aspectTooltip"_tr().c_str()
        )) {
        widgets::openAnchoredPopup("##viewport-aspect-menu");
    }
    if (widgets::beginAnchoredPopup("##viewport-aspect-menu")) {
        for (auto const& preset : widgets::kAspectPresets) {
            bool const current = std::abs(preset.ratio - mVideoAspectRatio) < 0.01f;
            // Routed through the editor so the choice reaches the persisted layout preferences.
            if (ImGui::MenuItem(preset.label, nullptr, current)) ctx.commands.setVideoAspectRatio(preset.ratio);
        }
        widgets::endAnchoredPopup();
    }

    ImGui::SameLine(0.0f, metrics::gutter() * 2.0f);
    ImGui::SetCursorPosY((height - ImGui::GetFrameHeight()) * 0.5f);
    bool autoPreview = mAutoPreview;
    if (ImGui::Checkbox(
            ("playback.refactorEditor.viewport.autoPreview"_tr() + "##viewport-auto-preview").c_str(),
            &autoPreview
        )) {
        ctx.commands.setAutoPreviewEnabled(autoPreview);
    }
    widgets::itemTooltip("playback.refactorEditor.viewport.autoPreviewTooltip"_tr().c_str());

    // Maximize sits with the transport controls; the camera-path toggle stays in the View menu.
    ImGui::SameLine(std::max(0.0f, width - metrics::iconButton() - metrics::gutter()));
    ImGui::SetCursorPosY((height - metrics::iconButton()) * 0.5f);
    if (widgets::iconToggle(
            "viewport-info",
            ICON_INFO,
            "playback.refactorEditor.viewport.infoOverlay"_tr().c_str(),
            mInfoOverlayVisible
        )) {
        ctx.commands.setInfoOverlayVisible(!mInfoOverlayVisible);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void ViewportPanel::drawInfoOverlay(PanelContext const& ctx, ImVec2 const& videoMin, ImDrawList* drawList) const {
    auto const& timeline = ctx.state.cameraTimeline;
    if (!timeline) return;

    auto const*                     selectedCamera = ctx.selection.getAs<state::editing::model::SelectedCamera>();
    visuals::ReplaySampleTime const time{std::max(0, ctx.state.currentTick), 1};
    auto const                      sample =
        selectedCamera ? timeline->sampleCameraById(selectedCamera->cameraId, time) : timeline->sample(time);
    if (!sample) return;

    auto const& pose = sample->state;
    char        position[64]{};
    char        rotation[64]{};
    char        lens[64]{};
    std::snprintf(
        position,
        sizeof(position),
        "%s  %.2f, %.2f, %.2f",
        "playback.refactorEditor.viewport.position"_tr().c_str(),
        pose.x,
        pose.y,
        pose.z
    );
    std::snprintf(
        rotation,
        sizeof(rotation),
        "%s  %.2f / %.2f",
        "playback.refactorEditor.viewport.rotation"_tr().c_str(),
        pose.yaw,
        pose.pitch
    );
    std::snprintf(
        lens,
        sizeof(lens),
        "%s",
        "playback.refactorEditor.viewport.lensOverlay"_tr(pose.fov, ctx.state.currentTick).c_str()
    );

    float const pad   = metrics::gutter();
    float const lineY = ImGui::GetFontSize() + 2.0f;
    float const widest =
        std::max({ImGui::CalcTextSize(position).x, ImGui::CalcTextSize(rotation).x, ImGui::CalcTextSize(lens).x});
    ImVec2 const boxMin{videoMin.x + pad * 1.5f, videoMin.y + pad * 1.5f};
    ImVec2 const boxMax{boxMin.x + widest + pad * 2.0f, boxMin.y + lineY * 3.0f + pad * 1.5f};
    drawList->AddRectFilled(boxMin, boxMax, theme::kOverlayBg, theme::kFrameRounding);
    drawList->AddRect(boxMin, boxMax, theme::kBorder, theme::kFrameRounding);
    drawList->AddText({boxMin.x + pad, boxMin.y + pad * 0.75f}, theme::kText, position);
    drawList->AddText({boxMin.x + pad, boxMin.y + pad * 0.75f + lineY}, theme::kText, rotation);
    drawList->AddText({boxMin.x + pad, boxMin.y + pad * 0.75f + lineY * 2.0f}, theme::kTextDim, lens);
}

void ViewportPanel::drawFloatingTransport(PanelContext const& ctx, ImVec2 const& sceneMin, ImVec2 const& sceneMax) {
    auto const&       state    = ctx.state;
    float const       unit     = metrics::iconButton();
    constexpr int     kButtons = 6;
    float const       gap      = 2.0f;
    float const       pad      = metrics::gutter();
    std::string const timecode = widgets::formatTick(state.currentTick) + " / " + widgets::formatTick(state.totalTicks);
    float const       textWidth = ImGui::CalcTextSize(timecode.c_str()).x;
    float const       capsuleW  = unit * kButtons + gap * (kButtons - 1) + textWidth + pad * 3.0f;
    float const       capsuleH  = unit + pad;
    ImVec2 const      capsuleMin{
        sceneMin.x + (sceneMax.x - sceneMin.x - capsuleW) * 0.5f,
        sceneMax.y - capsuleH - pad * 2.0f
    };
    ImVec2 const capsuleMax{capsuleMin.x + capsuleW, capsuleMin.y + capsuleH};
    // Published to the mouse hook so a click here stays with ImGui instead of grabbing the game camera.
    mOverlayRect = {capsuleMin, capsuleMax};

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(capsuleMin, capsuleMax, theme::kOverlayBg, capsuleH * 0.5f);
    drawList->AddRect(capsuleMin, capsuleMax, theme::kBorder, capsuleH * 0.5f);
    drawList->AddText(
        {capsuleMin.x + pad, capsuleMin.y + widgets::textOffsetInBox(capsuleH, timecode.c_str())},
        theme::kTextDim,
        timecode.c_str()
    );

    float const startX = capsuleMin.x + pad * 2.0f + textWidth;
    float const y      = capsuleMin.y + pad * 0.5f;
    using widgets::VectorIcon;
    auto const button = [&](char const* id, int index, VectorIcon icon, std::string const& tooltip) {
        ImGui::SetCursorScreenPos({startX + (unit + gap) * static_cast<float>(index), y});
        return widgets::vectorIconButton(id, icon, tooltip.c_str());
    };

    if (button("##fv-start", 0, VectorIcon::SkipStart, "playback.refactorEditor.timeline.skipToStart"_tr())) {
        ctx.commands.seekTo(
            state::editing::TimelineNavigation::resolveSmartEdgeTick(
                state.currentTick,
                state.totalTicks,
                ctx.commands.exportStartTick(),
                ctx.commands.exportEndTick(),
                false
            )
        );
    }
    if (button("##fv-back", 1, VectorIcon::StepBack, "playback.refactorEditor.timeline.previousTick"_tr()))
        ctx.commands.seekRelative(-1);
    if (button(
            "##fv-play",
            2,
            state.paused ? VectorIcon::Play : VectorIcon::Pause,
            state.paused ? "playback.refactorEditor.timeline.play"_tr() : "playback.refactorEditor.timeline.pause"_tr()
        ))
        ctx.submitAction({EditorActionType::TogglePause});
    if (button("##fv-forward", 3, VectorIcon::StepForward, "playback.refactorEditor.timeline.nextTick"_tr()))
        ctx.commands.seekRelative(1);
    if (button("##fv-end", 4, VectorIcon::SkipEnd, "playback.refactorEditor.timeline.skipToEnd"_tr())) {
        ctx.commands.seekTo(
            state::editing::TimelineNavigation::resolveSmartEdgeTick(
                state.currentTick,
                state.totalTicks,
                ctx.commands.exportStartTick(),
                ctx.commands.exportEndTick(),
                true
            )
        );
    }
    if (button("##fv-restore", 5, VectorIcon::Restore, "playback.refactorEditor.timeline.restore"_tr()))
        ctx.commands.toggleViewportMaximized();
}

void ViewportPanel::setGameTexture(ImTextureID texture) { mGameTexture = texture; }

void ViewportPanel::setVideoAspectRatio(float aspectRatio) { mVideoAspectRatio = std::max(0.1f, aspectRatio); }

} // namespace playback::editor::ui
