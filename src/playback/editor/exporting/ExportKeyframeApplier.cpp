#include "ExportKeyframeApplier.h"

#include "playback/Playback.h"
#include "playback/editor/keyframe/CameraTimelineEvaluator.h"

#include <utility>

namespace playback::editor::exporting {

ExportKeyframeApplier::~ExportKeyframeApplier() { reset(); }

void ExportKeyframeApplier::configure(
    editing::model::EditorStateExt const& project,
    std::optional<std::string>            cameraFallback
) {
    reset();
    if (project.cameras.empty()) {
        Playback::getInstance().getSelf().getLogger().warn("Export camera timeline skipped: project has no cameras");
        return;
    }
    size_t keyframeCount = 0;
    for (auto const& camera : project.cameras) keyframeCount += camera.keysByTick.size();
    mTimeline =
        std::make_shared<keyframe::CameraTimelineEvaluator>(project, std::nullopt, std::move(cameraFallback), true);
    keyframe::publishCameraTimeline(keyframe::CameraTimelineSource::Export, mTimeline);
    auto& logger = Playback::getInstance().getSelf().getLogger();
    logger.info("Export camera pose timeline ready (cameras={}, keyframes={})", project.cameras.size(), keyframeCount);
    for (auto const& camera : project.cameras) {
        if (camera.keysByTick.empty()) continue;
        auto const& first = *camera.keysByTick.begin();
        auto const& last  = *camera.keysByTick.rbegin();
        logger.info(
            "Export camera pose range (camera={}, firstTick={}, first=({}, {}, {}, yaw={}, pitch={}, roll={}), "
            "lastTick={}, last=({}, {}, {}, yaw={}, pitch={}, roll={}))",
            camera.id,
            first.first,
            first.second.position.x,
            first.second.position.y,
            first.second.position.z,
            first.second.yaw,
            first.second.pitch,
            first.second.roll,
            last.first,
            last.second.position.x,
            last.second.position.y,
            last.second.position.z,
            last.second.yaw,
            last.second.pitch,
            last.second.roll
        );
    }
}

void ExportKeyframeApplier::reset() {
    keyframe::clearCameraTimeline(keyframe::CameraTimelineSource::Export, mTimeline);
    mTimeline.reset();
}

} // namespace playback::editor::exporting
