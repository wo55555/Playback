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
    CameraRenderState                       state;
    std::string                             cameraId;
    state::editing::model::CameraKeyframeMode mode{state::editing::model::CameraKeyframeMode::Interpolated};
};

// The camera track that owns a tick: the highest one covering the tick with an active keyframe at or before it.
struct CameraShot {
    state::editing::model::CameraEntity const* camera{};
    state::editing::model::CameraKeyframeMode  mode{state::editing::model::CameraKeyframeMode::Interpolated};
    // Person keyframes only: the replay player or entity the key at or before the tick names. Empty means the recorder.
    std::string subject;
    bool        thirdPersonCustom{false};

    [[nodiscard]] explicit operator bool() const noexcept { return camera != nullptr; }
    [[nodiscard]] bool     isEvaluatorDriven() const noexcept {
        return camera != nullptr
            && (mode == state::editing::model::CameraKeyframeMode::Interpolated
                || (mode == state::editing::model::CameraKeyframeMode::ThirdPerson && thirdPersonCustom));
    }
};

// The view of replay player `subject` (empty is the recorder) at `time`; `thirdPerson` pulls the camera behind it
// like vanilla F5. Without a sampler, person tracks yield no sample and the preview follows the subject natively.
using SubjectViewSampler = std::function<
    std::optional<CameraRenderState>(visuals::ReplaySampleTime const&, std::string const& subject, bool thirdPerson)>;

// Thread-safe eye/anchor pose sampler for any replay player or entity (`subject` empty = recorder).
using EntityPoseSampler = std::function<
    std::optional<CameraRenderState>(visuals::ReplaySampleTime const&, std::string const& subject)>;

// Clips a camera ray from `anchor` to `target` against camera-colliding blocks.
using CameraRayClipper = std::function<
    state::editing::model::Vec3(state::editing::model::Vec3 const& anchor, state::editing::model::Vec3 const& target)>;

class CameraTimelineEvaluator {
private:
    struct SpeedSpan {
        int         startTick{};
        int         endTick{};
        long double speed{1.0L};
        long double startUnscaled{0.0L};
    };

    struct SegmentInterval {
        long double rStart{};
        long double rEnd{};
        long double uStart{};
        int         kStartTick{};
        int         kEndTick{};
        bool        ignoreClipSpeed{false};
    };

    struct CameraTrackSegment {
        size_t                       dimensionSegment{};
        int                          firstTick{};
        int                          lastTick{};
        KeyframeTrack                track;
        std::vector<SegmentInterval> intervals;
        bool                         hasUnscaledIntervals{false};
    };

    struct CustomThirdPersonTrackSegment {
        CameraTrackSegment segment;
        std::string        subject;
        bool               followRotation{false};
        bool               collideBlocks{true};
    };

    state::editing::model::EditorStateExt                                       mProject;
    std::optional<std::string>                                                  mManualCameraId;
    bool                                                                        mHoldOutsideTracks{};
    bool                                                                        mAnyCameraHasClips{false};
    std::vector<int>                                                            mDimensionTransitionTicks;
    std::vector<SpeedSpan>                                                      mSpeedSpans;
    long double                                                                 mSpeedSpanEndU{0.0L};
    std::unordered_map<std::string, std::vector<CameraTrackSegment>>            mKeyframeTracks;
    std::unordered_map<std::string, std::vector<CustomThirdPersonTrackSegment>> mCustomThirdPersonTracks;
    SubjectViewSampler                                                          mSubjectSampler;
    EntityPoseSampler                                                           mEntityPoseSampler;
    CameraRayClipper                                                            mCameraRayClipper;

private:
    void               buildSpeedSpans();
    [[nodiscard]] bool cameraCoversTick(state::editing::model::CameraEntity const& camera, int64_t tick) const noexcept;

    [[nodiscard]] long double
    mapSegmentSampleTick(CameraTrackSegment const& segment, long double replayTick) const noexcept;

    [[nodiscard]] std::optional<CameraRenderState> sampleKeyframes(
        state::editing::model::CameraEntity const& camera,
        long double                                tick,
        bool                                       clamp,
        bool                                       allowBeforeFirst    = false,
        visuals::ReplaySampleTime const*           sampleTimeForLookAt = nullptr
    ) const;

    [[nodiscard]] std::optional<CameraRenderState> sampleCustomThirdPerson(
        state::editing::model::CameraEntity const&   camera,
        visuals::ReplaySampleTime const&             time,
        bool                                         clamp,
        bool                                         allowBeforeFirst = false,
        state::editing::model::CameraKeyframe const* activeKey        = nullptr
    ) const;

    [[nodiscard]] std::optional<CameraTimelineEvaluation>
    sampleTrack(state::editing::model::CameraEntity const& camera, visuals::ReplaySampleTime const& time) const;

public:
    [[nodiscard]] size_t                  dimensionSegmentForTick(long double tick) const noexcept;
    [[nodiscard]] long double             unscaledTicksAt(long double replayTick) const noexcept;
    [[nodiscard]] long double             replayTickForUnscaled(long double unscaledTicks) const noexcept;
    [[nodiscard]] std::vector<int> const& dimensionTransitionTicks() const noexcept {
        return mDimensionTransitionTicks;
    }
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
        SubjectViewSampler                    subjectSampler           = {},
        EntityPoseSampler                     entityPoseSampler        = {},
        CameraRayClipper                      cameraRayClipper         = {}
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

// Resolves the viewport's active camera track ID. Follow mode owns the viewport when active (matching
// CameraRenderHooks), so no camera track is active even if a track covers `currentTick`.
[[nodiscard]] inline std::string
resolveActiveCameraId(bool followingRecordedPlayer, CameraTimelineEvaluator const* timeline, int64_t currentTick) {
    if (followingRecordedPlayer || !timeline) return {};
    if (auto const shot = timeline->shotAt(currentTick)) return shot.camera->id;
    return {};
}
} // namespace playback::keyframe
