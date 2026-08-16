#include "CameraBindingOps.h"

#include "SequenceOps.h"

#include <algorithm>

namespace playback::state::editing::CameraBindingOps {
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

model::SubActor* findSubActor(model::EditorStateExt& state, std::string const& id) {
    auto it =
        std::find_if(state.worldActor.subActors.begin(), state.worldActor.subActors.end(), [&](auto const& actor) {
            return actor.id == id;
        });
    return it == state.worldActor.subActors.end() ? nullptr : &*it;
}
} // namespace


std::string addFreeCamera(model::EditorStateExt& state, std::string const& name) {
    model::CameraEntity camera;
    camera.id   = makeCameraId(state);
    camera.name = name.empty() ? "Camera " + std::to_string(state.cameras.size() + 1) : name;
    state.cameras.push_back(camera);
    if (state.cameras.size() > 1 && state.sequence.empty()) state.sequence.push_back({"sequence", 0, state.totalTicks});
    return camera.id;
}

std::string createBindingCamera(model::EditorStateExt& state, std::string const& subActorId, std::string const& name) {
    auto* actor = findSubActor(state, subActorId);
    if (!actor) return {};

    model::CameraEntity camera;
    camera.id                = makeCameraId(state);
    camera.name              = name.empty() ? actor->name + " (bind)" : name;
    camera.bindingEntityUuid = actor->id;
    actor->boundCameraIds.push_back(camera.id);
    state.cameras.push_back(camera);
    if (state.cameras.size() > 1 && state.sequence.empty()) state.sequence.push_back({"sequence", 0, state.totalTicks});
    return camera.id;
}

bool unbindCamera(model::EditorStateExt& state, std::string const& cameraId) {
    auto it = std::find_if(state.cameras.begin(), state.cameras.end(), [&](auto const& camera) {
        return camera.id == cameraId;
    });
    if (it == state.cameras.end() || it->locked || state.cameras.size() <= 1) return false;

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
    if (it == state.cameras.end() || it->locked || state.cameras.size() <= 1) return false;

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

} // namespace playback::state::editing::CameraBindingOps
