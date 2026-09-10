#pragma once

#include "playback/keyframe/CameraRenderState.h"

#include <glm/mat4x4.hpp>

#include <optional>

namespace bgfx {
struct Frame;
}

namespace playback::editor::graphics {

struct RenderCameraProjection {
    ::glm::dmat4 viewProjection{1.0};
    float        nearClipW{0.001f};
};

[[nodiscard]] bool                                       hookCameraRender(bool enable);
[[nodiscard]] bool                                       isCameraRenderInstalled();
[[nodiscard]] std::optional<keyframe::CameraRenderState> currentRendererCameraState();
[[nodiscard]] std::optional<RenderCameraProjection>      currentRenderCameraProjection();
void                                                     useSubmittedCameraProjection(bgfx::Frame const* frame);

} // namespace playback::editor::graphics
