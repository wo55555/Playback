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
    bool                                         firstPerson,
    std::string                                  firstPersonSubject
) {
    reset();
    if (firstPerson) {
        auto& session       = replay::ReplaySession::getInstance();
        mSavedSubjectChoice = session.replaySubjectChoice();
        mSubjectChoiceSaved = true;
        session.setReplaySubjectChoice(std::move(firstPersonSubject));
        keyframe::setCameraSampleOverride(
            keyframe::CameraTimelineSource::Export,
            [](visuals::ReplaySampleTime const& time) -> std::optional<keyframe::CameraRenderState> {
                // Export skips the follow path, so refresh the sleep weight the hand hooks read here.
                visuals::sampleReplaySubjectViewWeight(time);
                auto const pose = visuals::sampleReplaySubjectPose(time);
                auto const fov  = visuals::sampleReplaySubjectFov(time);
                if (!pose || !fov) return std::nullopt;
                // The sampler returns an eye position: the recorder through its camera offset, anyone else at feet plus 1.62.
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
        // The camera sits inside the recorded player, so its own model would fill the view.
        visuals::setReplaySubjectHidden(visuals::SubjectHideReason::Export, true);
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
    if (mSubjectChoiceSaved) {
        mSubjectChoiceSaved = false;
        auto saved          = std::move(mSavedSubjectChoice);
        mSavedSubjectChoice.clear();
        replay::ReplaySession::getInstance().setReplaySubjectChoice(std::move(saved));
    }
    visuals::setReplaySubjectHidden(visuals::SubjectHideReason::Export, false);
    keyframe::setCameraSampleOverride(keyframe::CameraTimelineSource::Export, {});
    keyframe::clearCameraTimeline(keyframe::CameraTimelineSource::Export, mTimeline);
    mTimeline.reset();
}

} // namespace playback::exporting
