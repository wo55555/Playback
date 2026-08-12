#include "ExportKeyframeApplier.h"

#include "playback/Playback.h"
#include "playback/editor/keyframe/CameraTimelineEvaluator.h"

#include <utility>

namespace playback::editor::exporting {

ExportKeyframeApplier::~ExportKeyframeApplier() { reset(); }

void ExportKeyframeApplier::configure(
    editing::model::EditorStateExt const& project,
    std::optional<float>                  aspectRatio,
    std::optional<std::string>            cameraFallback
) {
    reset();
    if (project.cameras.empty()) {
        Playback::getInstance().getSelf().getLogger().warn("Export camera timeline skipped: project has no cameras");
        return;
    }
    size_t keyframeCount = 0;
    for (auto const& camera : project.cameras) keyframeCount += camera.keys.size();
    mTimeline = std::make_shared<keyframe::CameraTimelineEvaluator>(
        project,
        std::nullopt,
        std::move(cameraFallback)
    );
    keyframe::publishCameraTimeline(keyframe::CameraTimelineSource::Export, mTimeline, aspectRatio);
    auto& logger = Playback::getInstance().getSelf().getLogger();
    logger.info(
        "Export camera timeline ready (cameras={}, keyframes={}, aspectRatio={})",
        project.cameras.size(),
        keyframeCount,
        aspectRatio ? *aspectRatio : 0.0f
    );
    for (auto const& camera : project.cameras) {
        if (camera.keys.empty()) continue;
        auto const& first = camera.keys.front();
        auto const& last  = camera.keys.back();
        logger.info(
            "Export camera keyframe range (camera={}, firstTick={}, first=({}, {}, {}, yaw={}, pitch={}, fov={}), "
            "lastTick={}, last=({}, {}, {}, yaw={}, pitch={}, fov={})",
            camera.id,
            first.tick,
            first.position.x,
            first.position.y,
            first.position.z,
            first.yaw,
            first.pitch,
            first.fov,
            last.tick,
            last.position.x,
            last.position.y,
            last.position.z,
            last.yaw,
            last.pitch,
            last.fov
        );
    }
}

void ExportKeyframeApplier::reset() {
    keyframe::clearCameraTimeline(keyframe::CameraTimelineSource::Export, mTimeline);
    mTimeline.reset();
}

} // namespace playback::editor::exporting
