#include "CameraSampler.h"

#include <algorithm>

namespace playback::refactor::camera_motion {
namespace {
float interpolate(float from, float to, float ratio) { return from + (to - from) * ratio; }
}
CameraSample CameraSampler::sampleAt(const editor::CameraEntity& camera, int tick) {
    CameraSample sample; sample.source = camera.name;
    if (camera.kind == editor::CameraKind::Keyframe && !camera.keys.empty()) { auto keys = camera.keys; std::sort(keys.begin(), keys.end(), [](const auto& left, const auto& right) { return left.tick < right.tick; }); const auto& first = keys.front(); const auto& last = keys.back(); const auto* a = &first; const auto* b = &last; for (size_t index = 1; index < keys.size(); ++index) if (tick <= keys[index].tick) { a = &keys[index - 1]; b = &keys[index]; break; } float ratio = b->tick == a->tick ? 0.0f : std::clamp(static_cast<float>(tick - a->tick) / static_cast<float>(b->tick - a->tick), 0.0f, 1.0f); sample.position = {interpolate(a->position.x, b->position.x, ratio), interpolate(a->position.y, b->position.y, ratio), interpolate(a->position.z, b->position.z, ratio)}; sample.rotation = {interpolate(a->yaw, b->yaw, ratio), interpolate(a->pitch, b->pitch, ratio)}; sample.fov = interpolate(a->fov, b->fov, ratio); }
    else if (camera.kind == editor::CameraKind::Path && camera.path) { sample.rotation = camera.path->defaultRotation; sample.fov = camera.path->defaultFov; if (!camera.path->points.empty()) { auto point = std::find_if(camera.path->points.rbegin(), camera.path->points.rend(), [tick](const auto& value) { return value.tick <= tick; }); sample.position = point == camera.path->points.rend() ? camera.path->points.front().position : point->position; } }
    else if (camera.kind == editor::CameraKind::Rig && camera.rig) { sample.position = camera.rig->basePosition; sample.rotation = camera.rig->baseRotation; sample.fov = camera.rig->baseFov; }
    else if (camera.kind == editor::CameraKind::Preset && camera.preset) { sample.position = camera.preset->offset; sample.rotation = camera.preset->rotation; sample.fov = camera.preset->fov; }
    return sample;
}
}
