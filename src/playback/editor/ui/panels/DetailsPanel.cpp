#include "DetailsPanel.h"

#include "playback/editor/ui/ReplayEditor.h"

#include "imgui.h"

#include <algorithm>
#include <array>

namespace playback::editor::ui {

namespace {

template <typename T>
T const* findById(std::vector<T> const& values, std::string const& id) {
    auto const it = std::find_if(values.begin(), values.end(), [&id](T const& value) { return value.id == id; });
    return it == values.end() ? nullptr : &*it;
}

char const* cameraKindName(editing::model::CameraKind kind) {
    static constexpr std::array names{"Keyframe", "Path", "Rig", "Preset"};
    return names[std::clamp(static_cast<int>(kind), 0, static_cast<int>(names.size()) - 1)];
}

char const* easingName(editing::model::EasingType easing) {
    static constexpr std::array names{"Linear", "Ease In", "Ease Out", "Ease InOut"};
    return names[std::clamp(static_cast<int>(easing), 0, static_cast<int>(names.size()) - 1)];
}

void submit(EditorAction action) {
    ReplayEditor::getInstance().submitAction(std::move(action));
}

}

void DetailsPanel::draw() {
    auto& editor = ReplayEditor::getInstance();
    auto const& state = editor.state();
    auto const project = state.project;
    if (!project) {
        ImGui::TextDisabled("No replay project is active.");
        return;
    }

    auto const& selection = editor.selection();
    ImGui::TextUnformatted("Details");
    ImGui::Separator();

    if (auto const* selected = selection.getAs<editing::model::SelectedSequence>()) {
        (void)selected;
        ImGui::TextUnformatted("Camera Sequence");
        ImGui::Text("Segments: %zu", project->sequence.size());
        for (auto const& segment : project->sequence) {
            if (ImGui::Selectable((std::to_string(segment.startTick) + " - " + std::to_string(segment.endTick)).c_str())) editor.selection().select(editing::model::SelectedSequenceSegment{segment.id});
        }
        return;
    }

    if (auto const* selected = selection.getAs<editing::model::SelectedSequenceSegment>()) {
        auto const* segment = findById(project->sequence, selected->segmentId);
        if (!segment) {
            ImGui::TextDisabled("Sequence segment no longer exists.");
            return;
        }
        ImGui::TextUnformatted("Sequence Segment");
        ImGui::Text("Range: %d - %d", segment->startTick, segment->endTick);
        ImGui::BeginDisabled(segment->locked);
        char const* preview = "Automatic: first camera";
        if (auto const* camera = findById(project->cameras, segment->cameraId)) preview = camera->name.c_str();
        if (ImGui::BeginCombo("Camera", preview)) {
            if (ImGui::Selectable("Automatic: first camera", segment->cameraId.empty())) {
                EditorAction action{EditorActionType::BindSequenceCamera};
                action.id = segment->id;
                submit(std::move(action));
            }
            for (auto const& camera : project->cameras) {
                if (ImGui::Selectable(camera.name.c_str(), camera.id == segment->cameraId)) {
                    EditorAction action{EditorActionType::BindSequenceCamera};
                    action.id = segment->id;
                    action.secondaryId = camera.id;
                    submit(std::move(action));
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Split at Playhead", {-1.0f, 0.0f})) {
            EditorAction action{EditorActionType::SplitSequence};
            action.tick = state.currentTick;
            submit(std::move(action));
        }
        if (ImGui::Button("Delete Segment", {-1.0f, 0.0f})) {
            EditorAction action{EditorActionType::DeleteSequenceSegment};
            action.id = segment->id;
            submit(std::move(action));
        }
        ImGui::EndDisabled();
        return;
    }

    if (selection.getAs<editing::model::SelectedWorldActor>()) {
        ImGui::TextUnformatted("World Actor");
        ImGui::Text("Segments: %zu", project->worldActor.segments.size());
        for (auto const& segment : project->worldActor.segments) {
            if (ImGui::Selectable((std::to_string(segment.startTick) + " - " + std::to_string(segment.endTick)).c_str())) editor.selection().select(editing::model::SelectedWorldActorSegment{segment.id});
        }
        ImGui::Text("Actors: %zu", project->worldActor.subActors.size());
        return;
    }

    if (auto const* selected = selection.getAs<editing::model::SelectedWorldActorSegment>()) {
        auto const* segment = findById(project->worldActor.segments, selected->segmentId);
        if (!segment) {
            ImGui::TextDisabled("World actor segment no longer exists.");
            return;
        }
        ImGui::TextUnformatted("World Actor Segment");
        ImGui::Text("Range: %d - %d", segment->startTick, segment->endTick);
        ImGui::Text("Source Tick: %d", segment->sourceTick);
        ImGui::BeginDisabled(segment->locked);
        float speed = segment->speed;
        if (ImGui::SliderFloat("Speed", &speed, 0.1f, 10.0f, "%.2fx") && ImGui::IsItemDeactivatedAfterEdit()) {
            EditorAction action{EditorActionType::SetWorldActorSpeed};
            action.id = segment->id;
            action.speed = speed;
            submit(std::move(action));
        }
        if (ImGui::Button("Split at Playhead", {-1.0f, 0.0f})) {
            EditorAction action{EditorActionType::SplitWorldActor};
            action.tick = state.currentTick;
            submit(std::move(action));
        }
        if (ImGui::Button("Ripple Delete", {-1.0f, 0.0f})) {
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
            ImGui::TextDisabled("Sub actor no longer exists.");
            return;
        }
        ImGui::TextUnformatted("Sub Actor");
        ImGui::Text("Name: %s", actor->name.c_str());
        ImGui::Text("Bound Cameras: %zu", actor->boundCameraIds.size());
        if (ImGui::Button("Create Binding Camera", {-1.0f, 0.0f})) {
            EditorAction action{EditorActionType::CreateBindingCamera};
            action.id = actor->id;
            action.name = actor->name + " Camera";
            submit(std::move(action));
        }
        return;
    }

    if (auto const* selected = selection.getAs<editing::model::SelectedCamera>()) {
        auto const* camera = findById(project->cameras, selected->cameraId);
        if (!camera) {
            ImGui::TextDisabled("Camera no longer exists.");
            return;
        }
        ImGui::TextUnformatted("Camera");
        ImGui::Text("Name: %s", camera->name.c_str());
        ImGui::Text("Keyframes: %zu", camera->keys.size());
        ImGui::BeginDisabled(camera->locked);
        if (ImGui::BeginCombo("Kind", cameraKindName(camera->kind))) {
            for (int index = 0; index < 4; ++index) {
                auto kind = static_cast<editing::model::CameraKind>(index);
                if (ImGui::Selectable(cameraKindName(kind), kind == camera->kind)) {
                    EditorAction action{EditorActionType::SetCameraKind};
                    action.id = camera->id;
                    action.kind = index;
                    submit(std::move(action));
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Add Keyframe at Playhead", {-1.0f, 0.0f})) {
            EditorAction action{EditorActionType::AddCameraKeyframe};
            action.id = camera->id;
            action.tick = state.currentTick;
            submit(std::move(action));
        }
        for (auto const& key : camera->keys) {
            if (ImGui::Selectable(("Tick " + std::to_string(key.tick)).c_str())) editor.selection().select(editing::model::SelectedKeyframe{camera->id, key.id});
        }
        if (!camera->bindingEntityUuid.empty() && ImGui::Button("Unbind Camera", {-1.0f, 0.0f})) {
            EditorAction action{EditorActionType::UnbindCamera};
            action.id = camera->id;
            submit(std::move(action));
        }
        if (ImGui::Button("Delete Camera", {-1.0f, 0.0f})) {
            EditorAction action{EditorActionType::DeleteCamera};
            action.id = camera->id;
            submit(std::move(action));
        }
        ImGui::EndDisabled();
        return;
    }

    if (auto const* selected = selection.getAs<editing::model::SelectedKeyframe>()) {
        auto const* camera = findById(project->cameras, selected->trackId);
        auto const* key = camera ? findById(camera->keys, selected->keyframeId) : nullptr;
        if (!camera || !key) {
            ImGui::TextDisabled("Keyframe no longer exists.");
            return;
        }
        ImGui::TextUnformatted("Camera Keyframe");
        ImGui::Text("Camera: %s", camera->name.c_str());
        ImGui::BeginDisabled(camera->locked);
        int tick = key->tick;
        if (ImGui::InputInt("Tick", &tick) && ImGui::IsItemDeactivatedAfterEdit()) {
            EditorAction action{EditorActionType::MoveCameraKeyframe};
            action.id = camera->id;
            action.secondaryId = key->id;
            action.tick = tick;
            submit(std::move(action));
        }
        ImGui::Text("Easing: %s", easingName(key->easingType));
        if (ImGui::Button("Delete Keyframe", {-1.0f, 0.0f})) {
            EditorAction action{EditorActionType::DeleteCameraKeyframe};
            action.id = camera->id;
            action.secondaryId = key->id;
            submit(std::move(action));
        }
        ImGui::EndDisabled();
        return;
    }

    ImGui::TextDisabled("Select a sequence segment, world actor, camera, or keyframe.");
    ImGui::Spacing();
    ImGui::Text("Duration: %d ticks", project->totalTicks);
    if (ImGui::Button("Add Free Camera", {-1.0f, 0.0f})) {
        EditorAction action{EditorActionType::AddFreeCamera};
        action.name = "Camera " + std::to_string(project->cameras.size() + 1);
        submit(std::move(action));
    }
}

} // namespace playback::editor::ui
