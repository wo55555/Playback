#pragma once

namespace playback::editor::keyframe {

struct CameraRenderState {
    float x{};
    float y{};
    float z{};
    float yaw{};
    float pitch{};
    float roll{};
    float fov{90.0f};
};

} // namespace playback::editor::keyframe
