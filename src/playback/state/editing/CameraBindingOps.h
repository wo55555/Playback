#pragma once

#include "playback/state/editing/models/EditorStateExt.h"

#include <string>

namespace playback::state::editing::CameraBindingOps {
std::string addFreeCamera(model::EditorStateExt& state, const std::string& name);
bool        deleteCamera(model::EditorStateExt& state, const std::string& cameraId);
} // namespace playback::state::editing::CameraBindingOps
