#include "ExportKeyframeApplier.h"

#include "playback/Playback.h"
#include "playback/keyframe/CameraTimelineEvaluator.h"
#include "playback/keyframe/FirstPersonView.h"
#include "playback/replay/ReplaySession.h"
#include "playback/visuals/ReplayEntityInterpolator.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace playback::exporting {

ExportKeyframeApplier::~ExportKeyframeApplier() { reset(); }

void ExportKeyframeApplier::configure(
    state::editing::model::EditorStateExt const& project,
    std::optional<std::string>                   cameraFallback
) {
    reset();
    auto const firstPersonSegments = std::ranges::count(
        project.sequence,
        state::editing::model::kFirstPersonCameraId,
        &state::editing::model::SequenceSegment::cameraId
    );
    if (project.cameras.empty() && firstPersonSegments == 0) {
        Playback::getInstance().getSelf().getLogger().warn("Export camera timeline skipped: project has no cameras");
        return;
    }
    if (firstPersonSegments > 0) {
        // The export plays the editor's subject; restore it afterwards in case the replay dropped it mid-export.
        mSavedSubjectChoice = replay::ReplaySession::getInstance().replaySubjectChoice();
        mSubjectChoiceSaved = true;
    }
    size_t keyframeCount = 0;
    for (auto const& camera : project.cameras) keyframeCount += camera.keysByTick.size();
    auto dimensionTransitionTicks = replay::ReplaySession::getInstance().getDimensionTransitionTicks();
    mTimeline                     = std::make_shared<keyframe::CameraTimelineEvaluator>(
        project,
        std::nullopt,
        std::move(cameraFallback),
        true,
        std::move(dimensionTransitionTicks),
        // Pure sampling: the render hook refreshes the sleep weight for the frame it actually draws.
        [](visuals::ReplaySampleTime const& time) { return keyframe::sampleRecordedPlayerView(time); }
    );
    keyframe::publishCameraTimeline(keyframe::CameraTimelineSource::Export, mTimeline);
    Playback::getInstance().getSelf().getLogger().debug(
        "Export camera pose timeline ready (cameras={}, keyframes={}, firstPersonSegments={})",
        project.cameras.size(),
        keyframeCount,
        firstPersonSegments
    );
}

void ExportKeyframeApplier::reset() {
    if (mSubjectChoiceSaved) {
        mSubjectChoiceSaved = false;
        auto saved          = std::move(mSavedSubjectChoice);
        mSavedSubjectChoice.clear();
        replay::ReplaySession::getInstance().setReplaySubjectChoice(std::move(saved));
    }
    visuals::setReplaySubjectHidden(visuals::SubjectHideReason::Export, false);
    keyframe::clearCameraTimeline(keyframe::CameraTimelineSource::Export, mTimeline);
    mTimeline.reset();
}

} // namespace playback::exporting
