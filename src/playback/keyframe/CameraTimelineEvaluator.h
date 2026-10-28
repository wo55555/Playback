#pragma once

#include "CameraRenderState.h"
#include "KeyframeTrack.h"
#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/visuals/ReplaySampleTime.h"

#include <functional>
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

// Supplies the recorded player's view for sequence segments bound to the first-person camera.
using FirstPersonSampler = std::function<std::optional<CameraRenderState>(visuals::ReplaySampleTime const&)>;

class CameraTimelineEvaluator {
private:
    struct CameraTrackSegment {
        size_t        dimensionSegment{};
        int           lastTick{};
        KeyframeTrack track;
    };

    // Either a keyframed camera or the recorded player's view; both empty means nothing renders.
    struct CameraChoice {
        state::editing::model::CameraEntity const* camera{};
        bool                                       firstPerson{};
    };

    state::editing::model::EditorStateExt                            mProject;
    std::optional<std::string>                                       mCameraOverride;
    std::optional<std::string>                                       mCameraFallback;
    bool                                                             mHoldLastKeyframe{};
    std::vector<int>                                                 mDimensionTransitionTicks;
    std::unordered_map<std::string, std::vector<CameraTrackSegment>> mKeyframeTracks;
    FirstPersonSampler                                               mFirstPersonSampler;

private:
    [[nodiscard]] CameraChoice                               cameraForTick(int64_t tick) const;
    [[nodiscard]] state::editing::model::CameraEntity const* fallbackCamera() const;

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
        std::vector<int>                      dimensionTransitionTicks = {},
        FirstPersonSampler                    firstPersonSampler       = {}
    );

    // True when the sequence (or an override) asks for the first-person view at this tick, sampled or not.
    [[nodiscard]] bool isFirstPersonAt(int64_t tick) const;

    [[nodiscard]] std::optional<CameraTimelineEvaluation>

    sample(visuals::ReplaySampleTime const& time) const;

    [[nodiscard]] std::optional<CameraTimelineEvaluation>

    sampleCameraById(std::string_view cameraId, visuals::ReplaySampleTime const& time) const;
};
} // namespace playback::keyframe
