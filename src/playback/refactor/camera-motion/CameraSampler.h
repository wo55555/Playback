#pragma once

#include "playback/editor/editing/models/CameraEntity.h"

#include <string>

namespace playback::editor::camera_motion {
struct CameraSample { editing::model::Vec3 position{0, 80, 0}; editing::model::Vec2 rotation{}; float fov{90.0f}; std::string source; bool valid{true}; };
class CameraSampler { public: static CameraSample sampleAt(const editing::model::CameraEntity& camera, int tick); };
}
