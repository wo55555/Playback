#include "CameraBindingOps.h"

#include "SequenceOps.h"

#include <algorithm>

namespace playback::editor::editing::CameraBindingOps {
namespace {

std::string makeCameraId(model::EditorStateExt const& state) {
    size_t next = state.cameras.size() + 1;
    for (;;) {
        auto id = "camera_" + std::to_string(next++);
        if (std::none_of(state.cameras.begin(), state.cameras.end(), [&id](auto const& camera) {
                return camera.id == id;
            })) {
            return id;
        }
    }
}
std::string addFreeCamera(model::EditorStateExt& state, const std::string& name) { model::CameraEntity camera; camera.id = makeId(state, "camera_"); camera.name = name.empty() ? "Camera " + std::to_string(state.cameras.size() + 1) : name; state.cameras.push_back(camera); if (state.cameras.size() > 1 && state.sequence.empty()) state.sequence.push_back({"sequence", 0, state.totalTicks}); return camera.id; }
std::string createBindingCamera(model::EditorStateExt& state, const std::string& subActorId, const std::string& name) { auto* actor = findSubActor(state, subActorId); if (!actor) return {}; model::CameraEntity camera; camera.id = makeId(state, "camera_"); camera.name = name.empty() ? actor->name + " (bind)" : name; camera.kind = model::CameraKind::Preset; camera.preset = model::CameraPreset{.kind = model::PresetKind::FollowEntity, .bindingEntityUuid = actor->id}; camera.bindingEntityUuid = actor->id; camera.bindingMode = 3; camera.bindingDamping = 0.15f; actor->boundCameraIds.push_back(camera.id); state.cameras.push_back(camera); if (state.cameras.size() > 1 && state.sequence.empty()) state.sequence.push_back({"sequence", 0, state.totalTicks}); return camera.id; }
const model::CameraEntity* resolveCamera(const model::EditorStateExt& state, const std::string& cameraId) { auto it = std::find_if(state.cameras.begin(), state.cameras.end(), [&](const auto& camera) { return camera.id == cameraId; }); if (it != state.cameras.end()) return &*it; return state.cameras.empty() ? nullptr : &state.cameras.front(); }
bool unbindCamera(model::EditorStateExt& state, const std::string& cameraId) { auto it = std::find_if(state.cameras.begin(), state.cameras.end(), [&](const auto& camera) { return camera.id == cameraId; }); if (it == state.cameras.end() || it->locked) return false; for (auto& actor : state.worldActor.subActors) actor.boundCameraIds.erase(std::remove(actor.boundCameraIds.begin(), actor.boundCameraIds.end(), cameraId), actor.boundCameraIds.end()); it->bindingEntityUuid.clear(); it->bindingMode = 0; return true; }
bool deleteCamera(model::EditorStateExt& state, const std::string& cameraId) { auto it = std::find_if(state.cameras.begin(), state.cameras.end(), [&](const auto& camera) { return camera.id == cameraId; }); if (it == state.cameras.end() || it->locked || state.cameras.size() <= 1) return false; for (auto& actor : state.worldActor.subActors) actor.boundCameraIds.erase(std::remove(actor.boundCameraIds.begin(), actor.boundCameraIds.end(), cameraId), actor.boundCameraIds.end()); SequenceOps::clearDanglingRefs(state.sequence, cameraId); state.cameras.erase(it); return true; }
}

} // namespace

std::string addFreeCamera(model::EditorStateExt& state, std::string const& name) {
    model::CameraEntity camera;
    camera.id   = makeCameraId(state);
    camera.name = name.empty() ? "Camera " + std::to_string(state.cameras.size() + 1) : name;
    state.cameras.push_back(camera);
    return camera.id;
}

std::string createBindingCamera(model::EditorStateExt& state, std::string const& subActorId, std::string const& name) {
    auto* actor = findSubActor(state, subActorId);
    if (!actor) return {};

    model::CameraEntity camera;
    camera.id                = makeCameraId(state);
    camera.name              = name.empty() ? actor->name + " (bind)" : name;
    camera.kind              = model::CameraKind::Preset;
    camera.bindingEntityUuid = actor->id;
    actor->boundCameraIds.push_back(camera.id);
    state.cameras.push_back(camera);
    return camera.id;
}

bool unbindCamera(model::EditorStateExt& state, std::string const& cameraId) {
    auto it = std::find_if(state.cameras.begin(), state.cameras.end(), [&](auto const& camera) {
        return camera.id == cameraId;
    });
    if (it == state.cameras.end() || it->locked) return false;

    bool changed = !it->bindingEntityUuid.empty();
    for (auto& actor : state.worldActor.subActors) {
        auto const oldSize = actor.boundCameraIds.size();
        actor.boundCameraIds.erase(
            std::remove(actor.boundCameraIds.begin(), actor.boundCameraIds.end(), cameraId),
            actor.boundCameraIds.end()
        );
        changed = changed || actor.boundCameraIds.size() != oldSize;
    }
    if (!changed) return false;

    it->bindingEntityUuid.clear();
    return true;
}

bool deleteCamera(model::EditorStateExt& state, std::string const& cameraId) {
    auto it = std::find_if(state.cameras.begin(), state.cameras.end(), [&](auto const& camera) {
        return camera.id == cameraId;
    });
    if (it == state.cameras.end() || it->locked) return false;

    for (auto& actor : state.worldActor.subActors) {
        actor.boundCameraIds.erase(
            std::remove(actor.boundCameraIds.begin(), actor.boundCameraIds.end(), cameraId),
            actor.boundCameraIds.end()
        );
    }
    SequenceOps::clearDanglingRefs(state.sequence, cameraId);
    state.cameras.erase(it);
    return true;
}

} // namespace playback::editor::editing::CameraBindingOps
