#include "ExportKeyframeApplier.h"

#include "playback/Playback.h"
#include "playback/keyframe/CameraTimelineEvaluator.h"
#include "playback/replay/ReplaySession.h"
#include "playback/visuals/ReplayEntityInterpolator.h"

#include <utility>

namespace playback::exporting {

ExportKeyframeApplier::~ExportKeyframeApplier() { reset(); }

void ExportKeyframeApplier::configure(
    state::editing::model::EditorStateExt const& project,
    std::optional<std::string>                   cameraFallback,
    bool                                         firstPerson
) {
    reset();
    if (firstPerson) {
        keyframe::setCameraSampleOverride(
            keyframe::CameraTimelineSource::Export,
            [](visuals::ReplaySampleTime const& time) -> std::optional<keyframe::CameraRenderState> {
                auto const pose = visuals::sampleReplaySubjectPose(time);
                auto const fov  = visuals::sampleReplaySubjectFov(time);
                if (!pose || !fov) return std::nullopt;
                // The recorded player's position is already its eye position.
                return keyframe::CameraRenderState{
                    pose->position.x,
                    pose->position.y,
                    pose->position.z,
                    pose->yaw,
                    pose->pitch,
                    0.0f,
                    *fov,
                };
            }
        );
        Playback::getInstance().getSelf().getLogger().debug("Export camera follows the recorded player");
    }
    if (project.cameras.empty()) {
        Playback::getInstance().getSelf().getLogger().warn("Export camera timeline skipped: project has no cameras");
        return;
    }
    size_t keyframeCount = 0;
    for (auto const& camera : project.cameras) keyframeCount += camera.keysByTick.size();
    auto dimensionTransitionTicks = replay::ReplaySession::getInstance().getDimensionTransitionTicks();
    mTimeline                     = std::make_shared<keyframe::CameraTimelineEvaluator>(
        project,
        std::nullopt,
        std::move(cameraFallback),
        true,
        std::move(dimensionTransitionTicks)
    );
    keyframe::publishCameraTimeline(keyframe::CameraTimelineSource::Export, mTimeline);
    Playback::getInstance().getSelf().getLogger().debug(
        "Export camera pose timeline ready (cameras={}, keyframes={})",
        project.cameras.size(),
        keyframeCount
    );
}

void ExportKeyframeApplier::reset() {
    keyframe::setCameraSampleOverride(keyframe::CameraTimelineSource::Export, {});
    keyframe::clearCameraTimeline(keyframe::CameraTimelineSource::Export, mTimeline);
    mTimeline.reset();
}

} // namespace playback::exporting
