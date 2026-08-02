#include "DetailsPanel.h"

#include "playback/refactor/editor/Editor.h"
#include "playback/refactor/editor/EditorBridge.h"
#include "playback/refactor/editor/models/SelectionModel.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <string>

namespace playback::refactor::editor {
namespace {

template <typename T>
T* findById(std::vector<T>& values, const std::string& id) {
    auto it = std::find_if(values.begin(), values.end(), [&id](const T& value) { return value.id == id; });
    return it == values.end() ? nullptr : &*it;
}

const char* categoryName(SubActorCategory category) {
    switch (category) {
    case SubActorCategory::Players: return "Players";
    case SubActorCategory::Creatures: return "Creatures";
    case SubActorCategory::Entities: return "Entities";
    default: return "Default";
    }
}

const char* cameraKindName(CameraKind kind) {
    static constexpr std::array<const char*, 4> names{"Keyframe", "Path", "Rig", "Preset"};
    auto index = std::clamp(static_cast<int>(kind), 0, static_cast<int>(names.size()) - 1);
    return names[index];
}

const char* easingName(EasingType easing) {
    static constexpr std::array<const char*, 4> names{"Linear", "Ease In", "Ease Out", "Ease InOut"};
    auto index = std::clamp(static_cast<int>(easing), 0, static_cast<int>(names.size()) - 1);
    return names[index];
}

void beginLocked(bool locked) {
    if (locked) ImGui::BeginDisabled();
}

void endLocked(bool locked) {
    if (locked) ImGui::EndDisabled();
}

}

void DetailsPanel::draw() {
    auto& selection = Editor::getInstance().selection();
    if (!selection.hasSelection()) {
        drawEmpty();
    } else if (selection.getAs<SelectedSequence>()) {
        drawSequence();
    } else if (selection.getAs<SelectedSequenceSegment>()) {
        drawSequenceSegment();
    } else if (selection.getAs<SelectedWorldActor>()) {
        drawWorldActor();
    } else if (selection.getAs<SelectedWorldActorSegment>()) {
        drawWorldActorSegment();
    } else if (selection.getAs<SelectedSubActor>()) {
        drawSubActor();
    } else if (selection.getAs<SelectedCamera>()) {
        drawCamera();
    } else if (selection.getAs<SelectedKeyframe>()) {
        drawKeyframe();
    } else if (selection.getAs<SelectedMarker>()) {
        drawMarker();
    } else {
        drawEmpty();
    }
}

void DetailsPanel::drawEmpty() {
    auto& editor = Editor::getInstance();
    auto& state = editor.state();
    ImGui::Text("Details");
    ImGui::Separator();
    ImGui::TextDisabled("Select a sequence segment, world actor, camera, or marker.");
    ImGui::Spacing();
    ImGui::Text("Replay: %s", state.projectName.empty() ? "Untitled" : state.projectName.c_str());
    ImGui::Text("Duration: %d ticks", state.totalTicks);
    if (ImGui::Button("Add Free Camera", ImVec2(-1, 0))) {
        EditorBridge::getInstance().addFreeCamera(state, "Free Camera " + std::to_string(state.cameras.size() + 1));
    }
}

void DetailsPanel::drawSequence() {
    auto& editor = Editor::getInstance();
    auto& state = editor.state();
    ImGui::Text("Camera Sequence");
    ImGui::Separator();
    ImGui::Text("Duration: %d ticks", state.totalTicks);
    ImGui::Text("Segments: %zu", state.sequence.size());
    for (const auto& segment : state.sequence) {
        ImGui::PushID(segment.id.c_str());
        const char* cameraName = "Automatic: first camera";
        if (auto* camera = findById(state.cameras, segment.cameraId)) cameraName = camera->name.c_str();
        if (ImGui::Selectable((std::to_string(segment.startTick) + " - " + std::to_string(segment.endTick) + "  " + cameraName).c_str())) {
            editor.selection().select(SelectedSequenceSegment{segment.id});
        }
        ImGui::PopID();
    }
}

void DetailsPanel::drawSequenceSegment() {
    auto& editor = Editor::getInstance();
    auto* selected = editor.selection().getAs<SelectedSequenceSegment>();
    auto& state = editor.state();
    auto* segment = selected ? findById(state.sequence, selected->segmentId) : nullptr;
    if (!segment) {
        ImGui::TextDisabled("The selected sequence segment no longer exists.");
        return;
    }
    ImGui::Text("Sequence Segment");
    ImGui::Separator();
    ImGui::Text("Range: %d - %d", segment->startTick, segment->endTick);
    ImGui::Text("Duration: %d ticks", segment->endTick - segment->startTick);
    ImGui::Text("Locked: %s", segment->locked ? "Yes" : "No");
    beginLocked(segment->locked);
    const char* preview = "Automatic: first camera";
    if (auto* camera = findById(state.cameras, segment->cameraId)) preview = camera->name.c_str();
    if (ImGui::BeginCombo("Camera", preview)) {
        if (ImGui::Selectable("Automatic: first camera", segment->cameraId.empty())) {
            EditorBridge::getInstance().bindSequence(state, segment->id, "");
        }
        for (const auto& camera : state.cameras) {
            if (ImGui::Selectable(camera.name.c_str(), camera.id == segment->cameraId)) {
                EditorBridge::getInstance().bindSequence(state, segment->id, camera.id);
            }
        }
        ImGui::EndCombo();
    }
    if (state.cameras.empty()) ImGui::TextDisabled("No cameras exist; preview has no fallback camera.");
    if (ImGui::Button("Split at Playhead", ImVec2(-1, 0))) EditorBridge::getInstance().splitSequence(state, state.currentTick);
    if (ImGui::Button("Delete Segment", ImVec2(-1, 0))) EditorBridge::getInstance().deleteSequenceSegment(state, segment->id);
    endLocked(segment->locked);
}

void DetailsPanel::drawWorldActor() {
    auto& editor = Editor::getInstance();
    auto& actor = editor.state().worldActor;
    ImGui::Text("World Actor");
    ImGui::Separator();
    ImGui::Text("Replay: %s", actor.name.empty() ? "Unavailable" : actor.name.c_str());
    ImGui::Text("Duration: %d ticks", actor.totalTicks);
    ImGui::Text("Segments: %zu", actor.segments.size());
    for (const auto& segment : actor.segments) {
        if (ImGui::Selectable((std::to_string(segment.startTick) + " - " + std::to_string(segment.endTick)).c_str())) {
            editor.selection().select(SelectedWorldActorSegment{segment.id});
        }
    }
    constexpr std::array categories{SubActorCategory::Default, SubActorCategory::Players, SubActorCategory::Creatures, SubActorCategory::Entities};
    for (auto category : categories) {
        if (ImGui::CollapsingHeader(categoryName(category))) {
            bool hasActors = false;
            for (const auto& subActor : actor.subActors) {
                if (subActor.category != category) continue;
                hasActors = true;
                if (ImGui::Selectable(subActor.name.c_str())) editor.selection().select(SelectedSubActor{subActor.id});
            }
            if (!hasActors) ImGui::TextDisabled("No actors");
        }
    }
}

void DetailsPanel::drawWorldActorSegment() {
    auto& editor = Editor::getInstance();
    auto* selected = editor.selection().getAs<SelectedWorldActorSegment>();
    auto& state = editor.state();
    auto* segment = selected ? findById(state.worldActor.segments, selected->segmentId) : nullptr;
    if (!segment) {
        ImGui::TextDisabled("The selected world actor segment no longer exists.");
        return;
    }
    ImGui::Text("World Actor Segment");
    ImGui::Separator();
    ImGui::Text("Range: %d - %d", segment->startTick, segment->endTick);
    ImGui::Text("Source Tick: %d", segment->sourceTick);
    ImGui::Text("Duration: %d ticks", segment->endTick - segment->startTick);
    ImGui::Text("Locked: %s", segment->locked ? "Yes" : "No");
    beginLocked(segment->locked);
    float speed = segment->speed;
    ImGui::SliderFloat("Speed", &speed, 0.1f, 10.0f, "%.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        EditorBridge::getInstance().setWorldActorSegmentSpeed(state, segment->id, speed);
    }
    if (ImGui::Button("Split at Playhead", ImVec2(-1, 0))) EditorBridge::getInstance().splitWorldActor(state, state.currentTick);
    if (ImGui::Button("Ripple Delete", ImVec2(-1, 0))) EditorBridge::getInstance().rippleDeleteWorldActor(state, segment->id);
    endLocked(segment->locked);
}

void DetailsPanel::drawSubActor() {
    auto& editor = Editor::getInstance();
    auto* selected = editor.selection().getAs<SelectedSubActor>();
    auto& state = editor.state();
    auto* actor = selected ? findById(state.worldActor.subActors, selected->subActorId) : nullptr;
    if (!actor) {
        ImGui::TextDisabled("The selected sub actor no longer exists.");
        return;
    }
    ImGui::Text("Sub Actor");
    ImGui::Separator();
    ImGui::Text("Name: %s", actor->name.c_str());
    ImGui::Text("Category: %s", categoryName(actor->category));
    ImGui::Text("Position: %.2f, %.2f, %.2f", actor->position.x, actor->position.y, actor->position.z);
    ImGui::Text("Rotation: %.1f, %.1f", actor->rotation.x, actor->rotation.y);
    ImGui::Text("Bound Cameras: %zu", actor->boundCameraIds.size());
    for (const auto& [key, value] : actor->agentDetails) ImGui::Text("%s: %s", key.c_str(), value.c_str());
    if (actor->category == SubActorCategory::Players || actor->category == SubActorCategory::Creatures || actor->category == SubActorCategory::Entities) {
        if (ImGui::Button("Create Binding Camera", ImVec2(-1, 0))) {
            EditorBridge::getInstance().createBindingCamera(state, actor->id, actor->name + " Camera");
        }
    } else {
        ImGui::TextDisabled("This actor category cannot create binding cameras.");
    }
}

void DetailsPanel::drawCamera() {
    auto& editor = Editor::getInstance();
    auto* selected = editor.selection().getAs<SelectedCamera>();
    auto& state = editor.state();
    auto* camera = selected ? findById(state.cameras, selected->cameraId) : nullptr;
    if (!camera) {
        ImGui::TextDisabled("The selected camera no longer exists.");
        return;
    }
    ImGui::Text("Camera");
    ImGui::Separator();
    ImGui::Text("Name: %s", camera->name.c_str());
    ImGui::Text("Binding: %s", camera->bindingEntityUuid.empty() ? "Free" : camera->bindingEntityUuid.c_str());
    ImGui::Text("Locked: %s", camera->locked ? "Yes" : "No");
    beginLocked(camera->locked);
    if (ImGui::BeginCombo("Kind", cameraKindName(camera->kind))) {
        for (int index = 0; index < 4; ++index) {
            auto kind = static_cast<CameraKind>(index);
            if (ImGui::Selectable(cameraKindName(kind), kind == camera->kind)) EditorBridge::getInstance().setCameraKind(state, camera->id, kind);
        }
        ImGui::EndCombo();
    }
    ImGui::Text("Keyframes: %zu", camera->keys.size());
    if (ImGui::Button("Add Keyframe at Playhead", ImVec2(-1, 0))) EditorBridge::getInstance().addCameraKeyframe(state, camera->id, state.currentTick);
    for (const auto& key : camera->keys) {
        if (ImGui::Selectable(("Tick " + std::to_string(key.tick)).c_str())) editor.selection().select(SelectedKeyframe{camera->id, key.id});
    }
    if (!camera->bindingEntityUuid.empty() && ImGui::Button("Unbind Camera", ImVec2(-1, 0))) EditorBridge::getInstance().unbindCamera(state, camera->id);
    if (ImGui::Button("Delete Camera", ImVec2(-1, 0))) EditorBridge::getInstance().deleteCamera(state, camera->id);
    endLocked(camera->locked);
}

void DetailsPanel::drawKeyframe() {
    auto& editor = Editor::getInstance();
    auto* selected = editor.selection().getAs<SelectedKeyframe>();
    auto& state = editor.state();
    auto* camera = selected ? findById(state.cameras, selected->trackId) : nullptr;
    if (!camera) {
        ImGui::TextDisabled("The selected keyframe belongs to an unavailable camera.");
        return;
    }
    auto* key = findById(camera->keys, selected->keyframeId);
    if (!key) {
        ImGui::TextDisabled("The selected keyframe no longer exists.");
        return;
    }
    ImGui::Text("Camera Keyframe");
    ImGui::Separator();
    ImGui::Text("Camera: %s", camera->name.c_str());
    ImGui::Text("Position: %.2f, %.2f, %.2f", key->position.x, key->position.y, key->position.z);
    ImGui::Text("Rotation: %.1f, %.1f", key->yaw, key->pitch);
    ImGui::Text("FOV: %.1f", key->fov);
    beginLocked(camera->locked);
    int tick = key->tick;
    if (ImGui::InputInt("Tick", &tick) && ImGui::IsItemDeactivatedAfterEdit()) EditorBridge::getInstance().moveCameraKeyframe(state, camera->id, key->id, tick);
    if (ImGui::BeginCombo("Easing", easingName(key->easingType))) {
        for (int index = 0; index < 4; ++index) {
            auto easing = static_cast<EasingType>(index);
            if (ImGui::Selectable(easingName(easing), easing == key->easingType)) EditorBridge::getInstance().setKeyframeEasing(state, camera->id, key->id, easing);
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("Delete Keyframe", ImVec2(-1, 0))) EditorBridge::getInstance().deleteCameraKeyframe(state, camera->id, key->id);
    endLocked(camera->locked);
}

void DetailsPanel::drawMarker() {
    auto& editor = Editor::getInstance();
    auto* selected = editor.selection().getAs<SelectedMarker>();
    auto& state = editor.state();
    auto* marker = selected ? findById(state.markers, selected->markerId) : nullptr;
    if (!marker) {
        ImGui::TextDisabled("The selected marker no longer exists.");
        return;
    }
    ImGui::Text("Marker");
    ImGui::Separator();
    ImGui::Text("Label: %s", marker->label.c_str());
    ImGui::Text("Tick: %d", marker->tick);
    if (ImGui::Button("Delete Marker", ImVec2(-1, 0))) EditorBridge::getInstance().deleteMarker(state, marker->id);
}

}
