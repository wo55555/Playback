#pragma once

#include "playback/editor/editing/models/EditorStateExt.h"

#include <optional>
#include <string>

namespace playback::editor::editing::CameraBindingOps {
std::string addFreeCamera(model::EditorStateExt& state, const std::string& name);
std::string createBindingCamera(model::EditorStateExt& state, const std::string& subActorId, const std::string& name);
const model::CameraEntity* resolveCamera(const model::EditorStateExt& state, const std::string& cameraId);
bool unbindCamera(model::EditorStateExt& state, const std::string& cameraId);
bool deleteCamera(model::EditorStateExt& state, const std::string& cameraId);
}
