#pragma once

#include "CameraRenderState.h"
#include "KeyframeTrack.h"
#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/visuals/ReplaySampleTime.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace playback::keyframe {

struct CameraTimelineEvaluation {
    CameraRenderState state;
    std::string       cameraId;
};

class CameraTimelineEvaluator {
private:
    struct CameraTrackSegment {
        size_t        dimensionSegment{};
        int           lastTick{};
        KeyframeTrack track;
    };

    state::editing::model::EditorStateExt                            mProject;
    std::optional<std::string>                                       mCameraOverride;
    std::optional<std::string>                                       mCameraFallback;
    bool                                                             mHoldLastKeyframe{};
    std::vector<int>                                                 mDimensionTransitionTicks;
    std::unordered_map<std::string, std::vector<CameraTrackSegment>> mKeyframeTracks;

private:
    [[nodiscard]] state::editing::model::CameraEntity const* cameraForTick(int64_t tick) const;

    [[nodiscard]] std::optional<CameraRenderState>

    sampleCamera(state::editing::model::CameraEntity const& camera, long double tick) const;

public:
    [[nodiscard]] size_t dimensionSegmentForTick(long double tick) const noexcept;
    [[nodiscard]] std::span<state::editing::model::CameraEntity const> cameras() const noexcept {
        return mProject.cameras;
    }

    explicit CameraTimelineEvaluator(
        state::editing::model::EditorStateExt project,
        std::optional<std::string>            cameraOverride           = std::nullopt,
        std::optional<std::string>            cameraFallback           = std::nullopt,
        bool                                  holdLastKeyframe         = false,
        std::vector<int>                      dimensionTransitionTicks = {}
    );

    [[nodiscard]] std::optional<CameraTimelineEvaluation>

    sample(visuals::ReplaySampleTime const& time) const;

    [[nodiscard]] std::optional<CameraTimelineEvaluation>

    sampleCameraById(std::string_view cameraId, visuals::ReplaySampleTime const& time) const;
};
} // namespace playback::keyframe
