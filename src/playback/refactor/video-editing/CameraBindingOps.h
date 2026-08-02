#pragma once

#include "playback/refactor/editor/models/EditorStateExt.h"

#include <optional>
#include <string>

namespace playback::refactor::video_editing::CameraBindingOps {
std::string addFreeCamera(editor::EditorStateExt& state, const std::string& name);
std::string createBindingCamera(editor::EditorStateExt& state, const std::string& subActorId, const std::string& name);
const editor::CameraEntity* resolveCamera(const editor::EditorStateExt& state, const std::string& cameraId);
bool unbindCamera(editor::EditorStateExt& state, const std::string& cameraId);
bool deleteCamera(editor::EditorStateExt& state, const std::string& cameraId);
}
