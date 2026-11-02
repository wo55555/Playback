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
    CameraRenderState                      state;
    std::string                            cameraId;
    state::editing::model::CameraTrackKind kind{state::editing::model::CameraTrackKind::Keyframe};
};

// The camera track that owns a tick: the highest one in the list whose first and last keys cover it.
struct CameraShot {
    state::editing::model::CameraEntity const* camera{};
    // Person tracks only: the replay player the key at or before the tick names. Empty means the recorder.
    std::string subject;

    [[nodiscard]] explicit operator bool() const noexcept { return camera != nullptr; }
    [[nodiscard]] state::editing::model::CameraTrackKind kind() const noexcept {
        return camera ? camera->kind : state::editing::model::CameraTrackKind::Keyframe;
    }
};

// The view of replay player `subject` (empty is the recorder) at `time`; `thirdPerson` pulls the camera behind it
// like vanilla F5. Without a sampler, person tracks yield no sample and the preview follows the subject natively.
using SubjectViewSampler = std::function<
    std::optional<CameraRenderState>(visuals::ReplaySampleTime const&, std::string const& subject, bool thirdPerson)>;

class CameraTimelineEvaluator {
private:
    struct CameraTrackSegment {
        size_t        dimensionSegment{};
        int           firstTick{};
        int           lastTick{};
        KeyframeTrack track;
    };

    state::editing::model::EditorStateExt                            mProject;
    std::optional<std::string>                                       mManualCameraId;
    bool                                                             mHoldOutsideTracks{};
    std::vector<int>                                                 mDimensionTransitionTicks;
    std::unordered_map<std::string, std::vector<CameraTrackSegment>> mKeyframeTracks;
    SubjectViewSampler                                               mSubjectSampler;

private:
    [[nodiscard]] std::optional<CameraRenderState>
    sampleKeyframes(state::editing::model::CameraEntity const& camera, long double tick, bool clamp) const;

    [[nodiscard]] std::optional<CameraTimelineEvaluation>
    sampleTrack(state::editing::model::CameraEntity const& camera, visuals::ReplaySampleTime const& time) const;

public:
    [[nodiscard]] size_t dimensionSegmentForTick(long double tick) const noexcept;
    [[nodiscard]] std::span<state::editing::model::CameraEntity const> cameras() const noexcept {
        return mProject.cameras;
    }

    // `manualCameraId` renders, held at its nearest key, wherever no track is active. `holdOutsideTracks` then
    // falls back to the first keyframe track, so an export always has a camera.
    explicit CameraTimelineEvaluator(
        state::editing::model::EditorStateExt project,
        std::optional<std::string>            manualCameraId           = std::nullopt,
        bool                                  holdOutsideTracks        = false,
        std::vector<int>                      dimensionTransitionTicks = {},
        SubjectViewSampler                    subjectSampler           = {}
    );

    [[nodiscard]] CameraShot shotAt(int64_t tick) const;

    [[nodiscard]] std::optional<CameraTimelineEvaluation> sample(visuals::ReplaySampleTime const& time) const;

    // Keyframe tracks only, ignoring person tracks and the manual camera: the path the overlay draws. Never calls
    // the subject sampler, so the UI may use it.
    [[nodiscard]] std::optional<CameraTimelineEvaluation> sampleKeyframePath(visuals::ReplaySampleTime const& time
    ) const;

    [[nodiscard]] std::optional<CameraTimelineEvaluation>
    sampleCameraById(std::string_view cameraId, visuals::ReplaySampleTime const& time) const;
};
} // namespace playback::keyframe
