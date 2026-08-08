#pragma once

#include "CameraTimelineEvaluator.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace playback::editor::keyframe {

enum class CameraTimelineSource : uint8_t { Preview, Export };

using CameraTimelineHandle = std::shared_ptr<CameraTimelineEvaluator const>;

struct CameraTimelineSample {
    CameraRenderState state;
    std::optional<float> aspectRatio;
};

void publishCameraTimeline(
    CameraTimelineSource source,
    CameraTimelineHandle timeline,
    std::optional<float> aspectRatio = std::nullopt
);
void clearCameraTimeline(CameraTimelineSource source, CameraTimelineHandle const& expected = {});

[[nodiscard]] std::optional<CameraTimelineSample>
sampleCameraTimeline(CameraTimelineSource source, functions::render::ReplaySampleTime const& time) noexcept;

} // namespace playback::editor::keyframe
