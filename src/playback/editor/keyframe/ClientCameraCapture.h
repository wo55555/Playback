#pragma once

#include "CameraTimelineEvaluator.h"

#include <optional>

namespace playback::editor::keyframe {

struct CameraCaptureVector {
    float x{};
    float y{};
    float z{};
};

struct ClientCameraCapture {
    CameraRenderState   state;
    CameraCaptureVector nativeCameraPosition;
    CameraCaptureVector nativeCameraForward;
    CameraCaptureVector nativeCameraRight;
    CameraCaptureVector nativeCameraUp;
    CameraCaptureVector actorRotation;
    CameraCaptureVector actorForward;
    bool                usedNativeRotation{};
    bool                usedMatrixRotation{};
    bool                usedActorRotation{};
    bool                usedWorldCamera{};
    bool                usedWorldPosition{};
    bool                usedCameraActor{};
    bool                usedReplayPlayer{};
};

[[nodiscard]] std::optional<ClientCameraCapture> captureClientCamera() noexcept;

} // namespace playback::editor::keyframe
