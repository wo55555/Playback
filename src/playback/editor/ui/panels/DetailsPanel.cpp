#include "DetailsPanel.h"

#include "playback/editor/ui/ReplayEditor.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>

namespace playback::editor::ui {

using namespace ll::i18n_literals;

namespace {

template <typename T>
T const* findById(std::vector<T> const& values, std::string const& id) {
    auto const it = std::find_if(values.begin(), values.end(), [&id](T const& value) { return value.id == id; });
    return it == values.end() ? nullptr : &*it;
}

std::string cameraKindName(editing::model::CameraKind kind) {
    switch (kind) {
    case editing::model::CameraKind::Keyframe:
        return "playback.refactorEditor.details.cameraKind.keyframe"_tr();
    case editing::model::CameraKind::Path:
        return "playback.refactorEditor.details.cameraKind.path"_tr();
    case editing::model::CameraKind::Rig:
        return "playback.refactorEditor.details.cameraKind.rig"_tr();
    case editing::model::CameraKind::Preset:
        return "playback.refactorEditor.details.cameraKind.preset"_tr();
    }
    return "playback.refactorEditor.details.cameraKind.keyframe"_tr();
}

std::string easingName(editing::model::EasingType easing) {
    switch (easing) {
    case editing::model::EasingType::Linear:
        return "playback.refactorEditor.curve.linear"_tr();
    case editing::model::EasingType::EaseIn:
        return "playback.refactorEditor.curve.easeIn"_tr();
    case editing::model::EasingType::EaseOut:
        return "playback.refactorEditor.curve.easeOut"_tr();
    case editing::model::EasingType::EaseInOut:
        return "playback.refactorEditor.curve.easeInOut"_tr();
    }
    return "playback.refactorEditor.curve.linear"_tr();
}

void submit(EditorAction action) { ReplayEditor::getInstance().submitAction(std::move(action)); }

} // namespace

void DetailsPanel::draw() {
    auto&       editor  = ReplayEditor::getInstance();
    auto const& state   = editor.state();
    auto const  project = state.project;
    if (!project) {
        ImGui::TextDisabled("%s", "playback.refactorEditor.common.noActiveProject"_tr().c_str());
        return;
    }

    auto const& selection = editor.selection();
    ImGui::TextUnformatted("playback.refactorEditor.details.title"_tr().c_str());
    ImGui::Separator();

    if (auto const* selected = selection.getAs<editing::model::SelectedSequence>()) {
        (void)selected;
        ImGui::TextUnformatted("playback.refactorEditor.details.cameraSequence"_tr().c_str());
        ImGui::TextUnformatted("playback.refactorEditor.details.segments"_tr(project->sequence.size()).c_str());
        for (auto const& segment : project->sequence) {
            if (ImGui::Selectable((std::to_string(segment.startTick) + " - " + std::to_string(segment.endTick)).c_str()
                ))
                editor.selection().select(editing::model::SelectedSequenceSegment{segment.id});
        }
        return;
    }

    if (auto const* selected = selection.getAs<editing::model::SelectedSequenceSegment>()) {
        auto const* segment = findById(project->sequence, selected->segmentId);
        if (!segment) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.sequenceSegmentMissing"_tr().c_str());
            return;
        }
        ImGui::TextUnformatted("playback.refactorEditor.details.sequenceSegment"_tr().c_str());
        ImGui::TextUnformatted("playback.refactorEditor.details.range"_tr(segment->startTick, segment->endTick).c_str()
        );
        ImGui::BeginDisabled(segment->locked);
        std::string preview = "playback.refactorEditor.details.automaticFirstCamera"_tr();
        if (auto const* camera = findById(project->cameras, segment->cameraId)) preview = camera->name;
        if (ImGui::BeginCombo("playback.refactorEditor.details.camera"_tr().c_str(), preview.c_str())) {
            if (ImGui::Selectable(
                    "playback.refactorEditor.details.automaticFirstCamera"_tr().c_str(),
                    segment->cameraId.empty()
                )) {
                EditorAction action{EditorActionType::BindSequenceCamera};
                action.id = segment->id;
                submit(std::move(action));
            }
            for (auto const& camera : project->cameras) {
                if (ImGui::Selectable(camera.name.c_str(), camera.id == segment->cameraId)) {
                    EditorAction action{EditorActionType::BindSequenceCamera};
                    action.id          = segment->id;
                    action.secondaryId = camera.id;
                    submit(std::move(action));
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("playback.refactorEditor.context.clip.split"_tr().c_str(), {-1.0f, 0.0f})) {
            EditorAction action{EditorActionType::SplitSequence};
            action.tick = state.currentTick;
            submit(std::move(action));
        }
        if (ImGui::Button("playback.refactorEditor.details.deleteSegment"_tr().c_str(), {-1.0f, 0.0f})) {
            EditorAction action{EditorActionType::DeleteSequenceSegment};
            action.id = segment->id;
            submit(std::move(action));
        }
        ImGui::EndDisabled();
        return;
    }

    if (selection.getAs<editing::model::SelectedWorldActor>()) {
        ImGui::TextUnformatted("playback.refactorEditor.details.worldActor"_tr().c_str());
        ImGui::TextUnformatted(
            "playback.refactorEditor.details.segments"_tr(project->worldActor.segments.size()).c_str()
        );
        for (auto const& segment : project->worldActor.segments) {
            if (ImGui::Selectable((std::to_string(segment.startTick) + " - " + std::to_string(segment.endTick)).c_str()
                ))
                editor.selection().select(editing::model::SelectedWorldActorSegment{segment.id});
        }
        ImGui::SeparatorText("playback.refactorEditor.details.actors"_tr().c_str());
        if (project->worldActor.subActors.empty())
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.noActors"_tr().c_str());
        for (auto const& actor : project->worldActor.subActors) {
            ImGui::PushID(actor.id.c_str());
            if (ImGui::Selectable(actor.name.c_str())) {
                editor.selection().select(editing::model::SelectedSubActor{actor.id});
            }
            ImGui::PopID();
        }
        return;
    }

    if (auto const* selected = selection.getAs<editing::model::SelectedWorldActorSegment>()) {
        auto const* segment = findById(project->worldActor.segments, selected->segmentId);
        if (!segment) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.worldActorSegmentMissing"_tr().c_str());
            return;
        }
        ImGui::TextUnformatted("playback.refactorEditor.details.worldActorSegment"_tr().c_str());
        ImGui::TextUnformatted("playback.refactorEditor.details.range"_tr(segment->startTick, segment->endTick).c_str()
        );
        ImGui::TextUnformatted("playback.refactorEditor.details.sourceTick"_tr(segment->sourceTick).c_str());
        ImGui::BeginDisabled(segment->locked);
        if (mSpeedSegmentId != segment->id) {
            mSpeedSegmentId  = segment->id;
            mSpeedEditActive = false;
        }
        if (!mSpeedEditActive) mSpeedValue = segment->speed;
        ImGui::SliderFloat("playback.refactorEditor.details.speed"_tr().c_str(), &mSpeedValue, 0.1f, 10.0f, "%.2fx");
        if (ImGui::IsItemActivated()) mSpeedEditActive = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            EditorAction action{EditorActionType::SetWorldActorSpeed};
            action.id    = segment->id;
            action.speed = mSpeedValue;
            submit(std::move(action));
            mSpeedEditActive = false;
        } else if (ImGui::IsItemDeactivated()) {
            mSpeedEditActive = false;
        }
        if (ImGui::Button("playback.refactorEditor.context.clip.split"_tr().c_str(), {-1.0f, 0.0f})) {
            EditorAction action{EditorActionType::SplitWorldActor};
            action.tick = state.currentTick;
            submit(std::move(action));
        }
        if (ImGui::Button("playback.refactorEditor.details.rippleDelete"_tr().c_str(), {-1.0f, 0.0f})) {
            EditorAction action{EditorActionType::RippleDeleteWorldActorSegment};
            action.id = segment->id;
            submit(std::move(action));
        }
        ImGui::EndDisabled();
        return;
    }

    if (auto const* selected = selection.getAs<editing::model::SelectedSubActor>()) {
        auto const* actor = findById(project->worldActor.subActors, selected->subActorId);
        if (!actor) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.subActorMissing"_tr().c_str());
            return;
        }
        ImGui::TextUnformatted("playback.refactorEditor.details.subActor"_tr().c_str());
        ImGui::TextUnformatted("playback.refactorEditor.details.name"_tr(actor->name).c_str());
        ImGui::TextUnformatted("playback.refactorEditor.details.boundCameras"_tr(actor->boundCameraIds.size()).c_str());
        if (ImGui::Button("playback.refactorEditor.details.createBindingCamera"_tr().c_str(), {-1.0f, 0.0f})) {
            EditorAction action{EditorActionType::CreateBindingCamera};
            action.id   = actor->id;
            action.name = "playback.refactorEditor.details.bindingCameraName"_tr(actor->name);
            submit(std::move(action));
        }
        return;
    }

    if (auto const* selected = selection.getAs<editing::model::SelectedCamera>()) {
        auto const* camera = findById(project->cameras, selected->cameraId);
        if (!camera) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.cameraMissing"_tr().c_str());
            return;
        }
        ImGui::TextUnformatted("playback.refactorEditor.details.camera"_tr().c_str());
        ImGui::TextUnformatted("playback.refactorEditor.details.name"_tr(camera->name).c_str());
        ImGui::TextUnformatted("playback.refactorEditor.details.keyframes"_tr(camera->keys.size()).c_str());
        ImGui::BeginDisabled(camera->locked);
        std::string const selectedKindName = cameraKindName(camera->kind);
        if (ImGui::BeginCombo("playback.refactorEditor.details.kind"_tr().c_str(), selectedKindName.c_str())) {
            for (int index = 0; index < 4; ++index) {
                auto const        kind     = static_cast<editing::model::CameraKind>(index);
                std::string const kindName = cameraKindName(kind);
                if (ImGui::Selectable(kindName.c_str(), kind == camera->kind)) {
                    EditorAction action{EditorActionType::SetCameraKind};
                    action.id   = camera->id;
                    action.kind = index;
                    submit(std::move(action));
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("playback.refactorEditor.menu.addKeyframe"_tr().c_str(), {-1.0f, 0.0f})) {
            EditorAction action{EditorActionType::AddCameraKeyframe};
            action.id   = camera->id;
            action.tick = state.currentTick;
            submit(std::move(action));
        }
        for (auto const& key : camera->keys) {
            if (ImGui::Selectable("playback.refactorEditor.details.tickValue"_tr(key.tick).c_str()))
                editor.selection().select(editing::model::SelectedKeyframe{camera->id, key.id});
        }
        if (!camera->bindingEntityUuid.empty()
            && ImGui::Button("playback.refactorEditor.details.unbindCamera"_tr().c_str(), {-1.0f, 0.0f})) {
            EditorAction action{EditorActionType::UnbindCamera};
            action.id = camera->id;
            submit(std::move(action));
        }
        if (ImGui::Button("playback.refactorEditor.details.deleteCamera"_tr().c_str(), {-1.0f, 0.0f})) {
            EditorAction action{EditorActionType::DeleteCamera};
            action.id = camera->id;
            submit(std::move(action));
        }
        ImGui::EndDisabled();
        return;
    }

    if (auto const* selected = selection.getAs<editing::model::SelectedKeyframe>()) {
        auto const* camera = findById(project->cameras, selected->trackId);
        auto const* key    = camera ? findById(camera->keys, selected->keyframeId) : nullptr;
        if (!camera || !key) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.keyframeMissing"_tr().c_str());
            return;
        }
        ImGui::TextUnformatted("playback.refactorEditor.details.cameraKeyframe"_tr().c_str());
        ImGui::TextUnformatted("playback.refactorEditor.details.cameraValue"_tr(camera->name).c_str());
        ImGui::BeginDisabled(camera->locked);
        auto const keyframeId = camera->id + ":" + key->id;
        if (mTickKeyframeId != keyframeId) {
            mTickKeyframeId = keyframeId;
            mTickEditActive = false;
        }
        if (!mTickEditActive) mTickValue = key->tick;
        ImGui::InputInt("playback.refactorEditor.details.tick"_tr().c_str(), &mTickValue);
        if (ImGui::IsItemActivated()) mTickEditActive = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            EditorAction action{EditorActionType::MoveCameraKeyframe};
            action.id          = camera->id;
            action.secondaryId = key->id;
            action.tick        = mTickValue;
            submit(std::move(action));
            mTickEditActive = false;
        } else if (ImGui::IsItemDeactivated()) {
            mTickEditActive = false;
        }
        ImGui::TextUnformatted("playback.refactorEditor.details.easing"_tr(easingName(key->easingType)).c_str());
        if (ImGui::Button("playback.refactorEditor.details.deleteKeyframe"_tr().c_str(), {-1.0f, 0.0f})) {
            EditorAction action{EditorActionType::DeleteCameraKeyframe};
            action.id          = camera->id;
            action.secondaryId = key->id;
            submit(std::move(action));
        }
        ImGui::EndDisabled();
        return;
    }

    ImGui::TextDisabled("%s", "playback.refactorEditor.details.selectionHint"_tr().c_str());
    ImGui::Spacing();
    ImGui::TextUnformatted("playback.refactorEditor.details.duration"_tr(project->totalTicks).c_str());
    if (ImGui::Button("playback.refactorEditor.details.addFreeCamera"_tr().c_str(), {-1.0f, 0.0f})) {
        EditorAction action{EditorActionType::AddFreeCamera};
        action.name = "playback.refactorEditor.defaults.camera"_tr(project->cameras.size() + 1);
        submit(std::move(action));
    }
}

} // namespace playback::editor::ui
