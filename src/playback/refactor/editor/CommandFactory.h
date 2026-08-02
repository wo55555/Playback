#pragma once

#include "models/EditorStateExt.h"
#include "models/Track.h"

#include <memory>
#include <string>

namespace playback::refactor::editor {

// Forward declarations
class IEditCommand;

// ===== CommandFactory =====
// Creates IEditCommand instances for common editor operations.
// Used by EditorBridge and TimelinePanel to execute commands
// via the CommandStack.

class CommandFactory {
public:
    static std::unique_ptr<IEditCommand> createSplitSequence(int atTick);
    static std::unique_ptr<IEditCommand> createTrimSequence(const std::string& segmentId, int startTick, int endTick);
    static std::unique_ptr<IEditCommand> createDeleteSequenceSegment(const std::string& segmentId);
    static std::unique_ptr<IEditCommand> createBindSequenceToCamera(const std::string& segmentId, const std::string& cameraId);
    static std::unique_ptr<IEditCommand> createSplitWorldActor(int atTick);
    static std::unique_ptr<IEditCommand> createTrimWorldActor(const std::string& segmentId, int startTick, int endTick);
    static std::unique_ptr<IEditCommand> createSetWorldActorSpeed(const std::string& segmentId, float speed);
    static std::unique_ptr<IEditCommand> createRippleDeleteWorldActorSegment(const std::string& segmentId);
    static std::unique_ptr<IEditCommand> createAddFreeCamera(const std::string& name);
    static std::unique_ptr<IEditCommand> createDeleteCamera(const std::string& cameraId);
    static std::unique_ptr<IEditCommand> createCreateBindingCamera(const std::string& subActorId, const std::string& name);
    static std::unique_ptr<IEditCommand> createUnbindCamera(const std::string& cameraId);
    static std::unique_ptr<IEditCommand> createAddCameraKeyframe(const std::string& cameraId, int tick);
    static std::unique_ptr<IEditCommand> createMoveCameraKeyframe(const std::string& cameraId, const std::string& keyframeId, int tick);
    static std::unique_ptr<IEditCommand> createDeleteCameraKeyframe(const std::string& cameraId, const std::string& keyframeId);
    static std::unique_ptr<IEditCommand> createSetKeyframeEasing(const std::string& cameraId, const std::string& keyframeId, EasingType easing);
    static std::unique_ptr<IEditCommand> createSetCameraKind(const std::string& cameraId, CameraKind kind);
    static std::unique_ptr<IEditCommand> createSetSubActorDetails(const std::string& subActorId, AgentDetails details);

    // ── Clip commands ──
    static std::unique_ptr<IEditCommand> createSplitClip(
        const std::string& trackId, const std::string& clipId, int atTick);

    static std::unique_ptr<IEditCommand> createRemoveClip(
        const std::string& trackId, const std::string& clipId);

    static std::unique_ptr<IEditCommand> createTrimClip(
        const std::string& trackId, const std::string& clipId,
        int newInTick, int newOutTick);

    static std::unique_ptr<IEditCommand> createMoveClip(
        const std::string& trackId, const std::string& clipId, int newTrackTick);

    // ── Transition commands ──
    static std::unique_ptr<IEditCommand> createAddTransition(
        const std::string& fromClipId, const std::string& toClipId,
        TransitionKind kind, int durationTicks);

    // ── Track commands ──
    static std::unique_ptr<IEditCommand> createAddTrack(
        TrackKind kind, const std::string& name);

    static std::unique_ptr<IEditCommand> createRemoveTrack(
        const std::string& trackId);
};

} // namespace playback::refactor::editor
