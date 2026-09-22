#pragma once

#include "playback/keyframe/CameraRenderState.h"
#include "playback/visuals/ReplaySampleTime.h"

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <optional>

namespace bgfx {
struct Frame;
}

namespace playback::editor::graphics {

struct RenderCameraProjection {
    ::glm::dmat4 viewProjection{1.0};
    float        nearClipW{0.001f};
    // Sample time this view was rendered at, so overlays sample the timeline at the same instant.
    std::optional<visuals::ReplaySampleTime> sampleTime;
};

struct CameraRenderHookInventory {
    bool renderFrame{};
    bool upscaling{};
};

[[nodiscard]] bool                                       hookCameraRender(bool enable);
[[nodiscard]] bool                                       isCameraRenderInstalled();
[[nodiscard]] CameraRenderHookInventory                  cameraRenderHookInventory();
[[nodiscard]] std::optional<keyframe::CameraRenderState> currentRendererCameraState();
[[nodiscard]] std::optional<RenderCameraProjection>      currentRenderCameraProjection();
void                                                     useSubmittedCameraProjection(bgfx::Frame const* frame);

} // namespace playback::editor::graphics
