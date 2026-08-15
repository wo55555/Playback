#pragma once

namespace playback::keyframe {

struct CameraRenderState {
    float x{};
    float y{};
    float z{};
    float yaw{};
    float pitch{};
    float roll{};
    float fov{70.0f};
};

} // namespace playback::keyframe
