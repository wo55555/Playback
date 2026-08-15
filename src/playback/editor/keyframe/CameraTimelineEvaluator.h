#pragma once

#include "CameraRenderState.h"
#include "KeyframeTrack.h"
#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/functions/render/ReplaySampleTime.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace playback::editor::keyframe {

struct CameraTimelineEvaluation {
    CameraRenderState state;
    std::string       cameraId;
};

class CameraTimelineEvaluator {
private:
    editing::model::EditorStateExt                 mProject;
    std::optional<std::string>                     mCameraOverride;
    std::optional<std::string>                     mCameraFallback;
    bool                                           mHoldLastKeyframe{};
    std::unordered_map<std::string, KeyframeTrack> mKeyframeTracks;

private:
    [[nodiscard]] editing::model::CameraEntity const* cameraForTick(int64_t tick) const;

    [[nodiscard]] std::optional<CameraRenderState>

    sampleCamera(editing::model::CameraEntity const& camera, long double tick) const;

public:
    explicit CameraTimelineEvaluator(
        editing::model::EditorStateExt project,
        std::optional<std::string>     cameraOverride   = std::nullopt,
        std::optional<std::string>     cameraFallback   = std::nullopt,
        bool                           holdLastKeyframe = false
    );

    [[nodiscard]] std::optional<CameraTimelineEvaluation>

    sample(functions::render::ReplaySampleTime const& time) const;

    [[nodiscard]] std::optional<CameraTimelineEvaluation>

    sampleCameraById(std::string_view cameraId, functions::render::ReplaySampleTime const& time) const;
};
} // namespace playback::editor::keyframe
