#include "CameraBindingOps.h"

#include "SequenceOps.h"

#include <algorithm>

namespace playback::refactor::video_editing::CameraBindingOps {
namespace {
std::string makeId(const editor::EditorStateExt& state, std::string_view prefix) { return std::string(prefix) + std::to_string(state.cameras.size() + 1); }
editor::SubActor* findSubActor(editor::EditorStateExt& state, const std::string& id) { auto it = std::find_if(state.worldActor.subActors.begin(), state.worldActor.subActors.end(), [&](const auto& actor) { return actor.id == id; }); return it == state.worldActor.subActors.end() ? nullptr : &*it; }
}
std::string addFreeCamera(editor::EditorStateExt& state, const std::string& name) { editor::CameraEntity camera; camera.id = makeId(state, "camera_"); camera.name = name.empty() ? "Camera " + std::to_string(state.cameras.size() + 1) : name; state.cameras.push_back(camera); return camera.id; }
std::string createBindingCamera(editor::EditorStateExt& state, const std::string& subActorId, const std::string& name) { auto* actor = findSubActor(state, subActorId); if (!actor) return {}; editor::CameraEntity camera; camera.id = makeId(state, "camera_"); camera.name = name.empty() ? actor->name + " (bind)" : name; camera.kind = editor::CameraKind::Preset; camera.preset = editor::CameraPreset{.kind = editor::PresetKind::FollowEntity, .bindingEntityUuid = actor->id}; camera.bindingEntityUuid = actor->id; camera.bindingMode = 3; camera.bindingDamping = 0.15f; actor->boundCameraIds.push_back(camera.id); state.cameras.push_back(camera); return camera.id; }
const editor::CameraEntity* resolveCamera(const editor::EditorStateExt& state, const std::string& cameraId) { auto it = std::find_if(state.cameras.begin(), state.cameras.end(), [&](const auto& camera) { return camera.id == cameraId; }); if (it != state.cameras.end()) return &*it; return state.cameras.empty() ? nullptr : &state.cameras.front(); }
bool unbindCamera(editor::EditorStateExt& state, const std::string& cameraId) { auto it = std::find_if(state.cameras.begin(), state.cameras.end(), [&](const auto& camera) { return camera.id == cameraId; }); if (it == state.cameras.end() || it->locked) return false; for (auto& actor : state.worldActor.subActors) actor.boundCameraIds.erase(std::remove(actor.boundCameraIds.begin(), actor.boundCameraIds.end(), cameraId), actor.boundCameraIds.end()); it->bindingEntityUuid.clear(); it->bindingMode = 0; return true; }
bool deleteCamera(editor::EditorStateExt& state, const std::string& cameraId) { auto it = std::find_if(state.cameras.begin(), state.cameras.end(), [&](const auto& camera) { return camera.id == cameraId; }); if (it == state.cameras.end() || it->locked) return false; for (auto& actor : state.worldActor.subActors) actor.boundCameraIds.erase(std::remove(actor.boundCameraIds.begin(), actor.boundCameraIds.end(), cameraId), actor.boundCameraIds.end()); SequenceOps::clearDanglingRefs(state.sequence, cameraId); state.cameras.erase(it); return true; }
}
