#include "ExportKeyframeApplier.h"

#include "playback/Playback.h"
#include "playback/keyframe/CameraTimelineEvaluator.h"
#include "playback/keyframe/FirstPersonView.h"
#include "playback/replay/ReplaySession.h"
#include "playback/visuals/ReplayEntityInterpolator.h"

#include <utility>

namespace playback::exporting {

ExportKeyframeApplier::~ExportKeyframeApplier() { reset(); }

void ExportKeyframeApplier::configure(
    state::editing::model::EditorStateExt const& project,
    std::optional<std::string>                   cameraFallback
) {
    reset();
    if (project.cameras.empty()) {
        Playback::getInstance().getSelf().getLogger().warn("Export camera timeline skipped: project has no cameras");
        return;
    }
    size_t keyframeCount    = 0;
    size_t personTrackCount = 0;
    for (auto const& camera : project.cameras) {
        keyframeCount += camera.keysByTick.size();
        if (camera.kind != state::editing::model::CameraTrackKind::Keyframe) ++personTrackCount;
    }
    auto& session                  = replay::ReplaySession::getInstance();
    auto  dimensionTransitionTicks = session.getDimensionTransitionTicks();
    mTimeline                      = std::make_shared<keyframe::CameraTimelineEvaluator>(
        project,
        std::move(cameraFallback),
        true,
        std::move(dimensionTransitionTicks),
        // Binds the track's subject, then samples it. The render hook refreshes the sleep weight for the frame it
        // actually draws.
        [&session](visuals::ReplaySampleTime const& time, std::string const& subject, bool thirdPerson)
            -> std::optional<keyframe::CameraRenderState> {
            session.setShotSubject(subject);
            auto state = keyframe::sampleRecordedPlayerView(time);
            if (state && thirdPerson) state = session.thirdPersonView(*state);
            return state;
        }
    );
    keyframe::publishCameraTimeline(keyframe::CameraTimelineSource::Export, mTimeline);
    Playback::getInstance().getSelf().getLogger().debug(
        "Export camera pose timeline ready (cameras={}, keyframes={}, personTracks={})",
        project.cameras.size(),
        keyframeCount,
        personTrackCount
    );
}

void ExportKeyframeApplier::reset() {
    visuals::setReplaySubjectHidden(visuals::SubjectHideReason::Export, false);
    // The export left the follow view, so nothing is in third person any more.
    visuals::setReplaySubjectThirdPerson(false);
    keyframe::clearCameraTimeline(keyframe::CameraTimelineSource::Export, mTimeline);
    // Person tracks pinned their subjects; the user's choice comes back.
    if (mTimeline) replay::ReplaySession::getInstance().setShotSubject(std::nullopt);
    mTimeline.reset();
}

} // namespace playback::exporting
