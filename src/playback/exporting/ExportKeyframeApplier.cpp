#include "ExportKeyframeApplier.h"

#include "playback/Playback.h"
#include "playback/editor/graphics/CameraRenderHooks.h"
#include "playback/keyframe/CameraTimelineEvaluator.h"
#include "playback/replay/ReplaySession.h"
#include "playback/visuals/ReplayEntityInterpolator.h"

#include <cmath>
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
        // The recorded player's view carries no field of view, so keep the one the renderer is using now.
        float fov = 70.0f;
        if (auto const camera = editor::graphics::currentRendererCameraState()) {
            if (std::isfinite(camera->fov) && camera->fov > 1.0f && camera->fov < 179.0f) fov = camera->fov;
        }
        keyframe::setCameraSampleOverride(
            keyframe::CameraTimelineSource::Export,
            [fov](visuals::ReplaySampleTime const& time) -> std::optional<keyframe::CameraRenderState> {
                auto const pose = visuals::sampleReplaySubjectPose(time);
                if (!pose) return std::nullopt;
                // The recorded player's position is already its eye position.
                return keyframe::CameraRenderState{
                    pose->position.x,
                    pose->position.y,
                    pose->position.z,
                    pose->yaw,
                    pose->pitch,
                    0.0f,
                    fov,
                };
            }
        );
        Playback::getInstance().getSelf().getLogger().debug("Export camera follows the recorded player (fov={})", fov);
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
