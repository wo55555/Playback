#include "FirstPersonView.h"

#include "playback/visuals/ReplayEntityInterpolator.h"

#include <cmath>

namespace playback::keyframe {

std::optional<CameraRenderState> sampleRecordedPlayerView(visuals::ReplaySampleTime const& time) {
    if (!time.isValid()) return std::nullopt;
    auto const pose = visuals::sampleReplaySubjectPose(time);
    if (!pose) return std::nullopt;
    auto const& position = pose->position;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)
        || !std::isfinite(pose->yaw) || !std::isfinite(pose->pitch)
        || (position.x == 0.0f && position.y == 0.0f && position.z == 0.0f)) {
        return std::nullopt;
    }
    // The recorded player's position is already its eye position; the recorded FOV carries the spyglass zoom.
    auto const fov = visuals::sampleReplaySubjectFov(time);
    return CameraRenderState{
        position.x,
        position.y,
        position.z,
        pose->yaw,
        pose->pitch,
        0.0f,
        fov && *fov > 1.0f && *fov < 179.0f ? *fov : 70.0f,
    };
}

} // namespace playback::keyframe
