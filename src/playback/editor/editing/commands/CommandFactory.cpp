#include "CommandFactory.h"

#include "playback/editor/editing/commands/CameraCommands.h"
#include "playback/editor/editing/commands/EditingCommands.h"
#include "playback/editor/editing/commands/SequenceCommands.h"
#include "playback/editor/editing/commands/WorldActorCommands.h"

namespace playback::editor::editing::command {

std::unique_ptr<model::IEditCommand> CommandFactory::createSplitSequence(int atTick) {
    return std::make_unique<SplitSequenceAtPlayhead>(atTick);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createTrimSequence(const std::string& id, int start, int end) {
    return std::make_unique<TrimSequenceSegment>(id, start, end);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createDeleteSequenceSegment(const std::string& id) {
    return std::make_unique<DeleteSequenceSegment>(id);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createBindSequenceToCamera(const std::string& id, const std::string& cameraId) {
    return std::make_unique<BindSequenceToCamera>(id, cameraId);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSplitWorldActor(int tick) {
    return std::make_unique<SplitWorldActorAtPlayhead>(tick);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createTrimWorldActor(const std::string& id, int start, int end) {
    return std::make_unique<TrimWorldActorSegment>(id, start, end);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSetWorldActorSpeed(const std::string& id, float speed) {
    return std::make_unique<SetWorldActorSegmentSpeed>(id, speed);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createRippleDeleteWorldActorSegment(const std::string& id) {
    return std::make_unique<RippleDeleteWorldActorSeg>(id);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createAddFreeCamera(const std::string& name) {
    return std::make_unique<AddFreeCamera>(name);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createDeleteCamera(const std::string& id) {
    return std::make_unique<DeleteCamera>(id);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createCreateBindingCamera(const std::string& id, const std::string& name) {
    return std::make_unique<CreateBindingCamera>(id, name);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createUnbindCamera(const std::string& id) {
    return std::make_unique<UnbindCamera>(id);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createAddCameraKeyframe(const std::string& id, int tick) {
    return std::make_unique<AddKeyframe>(id, tick);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createMoveCameraKeyframe(const std::string& id, const std::string& keyframeId, int tick) {
    return std::make_unique<MoveKeyframe>(id, keyframeId, tick);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createDeleteCameraKeyframe(const std::string& id, const std::string& keyframeId) {
    return std::make_unique<DeleteKeyframe>(id, keyframeId);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetCameraKind(const std::string& id, model::CameraKind kind) {
    return std::make_unique<SetCameraKind>(id, kind);
}

// ===== Clip commands =====

std::unique_ptr<model::IEditCommand>
CommandFactory::createSplitClip(const std::string& trackId, const std::string& clipId, int atTick) {
    return std::make_unique<SplitClipCommand>(trackId, clipId, atTick);
}

std::unique_ptr<model::IEditCommand>
CommandFactory::createRemoveClip(const std::string& trackId, const std::string& clipId) {
    return std::make_unique<RemoveClipCommand>(trackId, clipId);
}

std::unique_ptr<model::IEditCommand>
CommandFactory::createTrimClip(const std::string& trackId, const std::string& clipId, int newInTick, int newOutTick) {
    return std::make_unique<TrimClipCommand>(trackId, clipId, newInTick, newOutTick);
}

std::unique_ptr<model::IEditCommand>
CommandFactory::createMoveClip(const std::string& trackId, const std::string& clipId, int newTrackTick) {
    return std::make_unique<MoveClipCommand>(trackId, clipId, newTrackTick);
}

// ===== Transition commands =====

std::unique_ptr<model::IEditCommand> CommandFactory::createAddTransition(
    const std::string&    fromClipId,
    const std::string&    toClipId,
    model::TransitionKind kind,
    int                   durationTicks
) {
    return std::make_unique<AddTransitionCommand>(fromClipId, toClipId, kind, durationTicks);
}

// ===== Track commands =====

std::unique_ptr<model::IEditCommand> CommandFactory::createAddTrack(model::TrackKind kind, const std::string& name) {
    // Placeholder — AddTrackCommand will be defined in future iterations
    (void)kind;
    (void)name;
    return nullptr;
}

std::unique_ptr<model::IEditCommand> CommandFactory::createRemoveTrack(const std::string& trackId) {
    // Placeholder — RemoveTrackCommand will be defined in future iterations
    (void)trackId;
    return nullptr;
}

} // namespace playback::editor::editing::command
