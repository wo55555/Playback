#include "CameraBindingOps.h"

#include "ll/api/i18n/I18n.h"

#include <algorithm>

namespace playback::state::editing::CameraBindingOps {

using namespace ll::i18n_literals;

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

// Counting cameras would reuse a name after a delete, so probe upwards until the name is free.
std::string defaultCameraName(model::EditorStateExt const& state) {
    for (size_t next = state.cameras.size() + 1;; ++next) {
        auto name = "playback.refactorEditor.defaults.camera"_tr(next);
        if (std::none_of(state.cameras.begin(), state.cameras.end(), [&name](auto const& camera) {
                return camera.name == name;
            })) {
            return name;
        }
    }
}
} // namespace


std::string addFreeCamera(model::EditorStateExt& state, std::string const& name) {
    model::CameraEntity camera;
    camera.id   = makeCameraId(state);
    camera.name = name.empty() ? defaultCameraName(state) : name;
    state.cameras.insert(state.cameras.begin(), camera);
    return camera.id;
}

bool deleteCamera(model::EditorStateExt& state, std::string const& cameraId) {
    auto it = std::find_if(state.cameras.begin(), state.cameras.end(), [&](auto const& camera) {
        return camera.id == cameraId;
    });
    if (it == state.cameras.end() || it->locked || state.cameras.size() <= 1) return false;

    state.cameras.erase(it);
    return true;
}

} // namespace playback::state::editing::CameraBindingOps
