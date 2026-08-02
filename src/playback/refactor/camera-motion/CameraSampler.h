#pragma once

#include "playback/refactor/editor/models/CameraEntity.h"

#include <string>

namespace playback::refactor::camera_motion {
struct CameraSample { editor::Vec3 position{0, 80, 0}; editor::Vec2 rotation{}; float fov{90.0f}; std::string source; bool valid{true}; };
class CameraSampler { public: static CameraSample sampleAt(const editor::CameraEntity& camera, int tick); };
}
