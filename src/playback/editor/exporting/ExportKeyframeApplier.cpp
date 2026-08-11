#include "ExportKeyframeApplier.h"

#include "playback/editor/keyframe/CameraTimelineEvaluator.h"

namespace playback::editor::exporting {

ExportKeyframeApplier::~ExportKeyframeApplier() { reset(); }

void ExportKeyframeApplier::configure(
    editing::model::EditorStateExt const& project,
    std::optional<float>                  aspectRatio
) {
    reset();
    if (project.cameras.empty()) return;
    mTimeline = std::make_shared<keyframe::CameraTimelineEvaluator>(project);
    keyframe::publishCameraTimeline(keyframe::CameraTimelineSource::Export, mTimeline, aspectRatio);
}

void ExportKeyframeApplier::reset() {
    keyframe::clearCameraTimeline(keyframe::CameraTimelineSource::Export, mTimeline);
    mTimeline.reset();
}

} // namespace playback::editor::exporting
