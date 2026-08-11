#include "ExportKeyframeApplier.h"

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
    if (project.cameras.empty()) return;
    mTimeline = std::make_shared<keyframe::CameraTimelineEvaluator>(
        project,
        std::nullopt,
        std::move(cameraFallback)
    );
    keyframe::publishCameraTimeline(keyframe::CameraTimelineSource::Export, mTimeline, aspectRatio);
}

void ExportKeyframeApplier::reset() {
    keyframe::clearCameraTimeline(keyframe::CameraTimelineSource::Export, mTimeline);
    mTimeline.reset();
}

} // namespace playback::editor::exporting
