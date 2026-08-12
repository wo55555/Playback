#pragma once

#include "CameraRenderState.h"
#include "KeyframeTrack.h"
#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/functions/render/ReplaySampleTime.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace playback::editor::keyframe {

struct CameraPathSampleRange {
    int startTick{};
    int endTick{};
    std::vector<CameraRenderState> samples;
};

class CameraTimelineEvaluator {
public:
    explicit CameraTimelineEvaluator(
        editing::model::EditorStateExt project,
        std::optional<std::string>     cameraOverride = std::nullopt,
        std::optional<std::string>     cameraFallback = std::nullopt
    );

    [[nodiscard]] std::optional<CameraRenderState> sample(functions::render::ReplaySampleTime const& time) const;
    [[nodiscard]] std::optional<CameraRenderState>
    sampleCameraById(std::string_view cameraId, functions::render::ReplaySampleTime const& time) const;
    [[nodiscard]] std::optional<CameraPathSampleRange> sampleCameraPathAround(
        std::string_view cameraId,
        functions::render::ReplaySampleTime const& time,
        size_t maxSamples
    ) const;

private:
    [[nodiscard]] editing::model::CameraEntity const* cameraForTick(int64_t tick) const;
    [[nodiscard]] std::optional<CameraRenderState>
    sampleCamera(editing::model::CameraEntity const& camera, long double tick) const;

    editing::model::EditorStateExt mProject;
    std::optional<std::string>     mCameraOverride;
    std::optional<std::string>     mCameraFallback;
    std::unordered_map<std::string, KeyframeTrack> mKeyframeTracks;
};

} // namespace playback::editor::keyframe
