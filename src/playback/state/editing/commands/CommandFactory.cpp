#include "CommandFactory.h"

#include "playback/state/editing/commands/CameraCommands.h"
#include "playback/state/editing/commands/EditingCommands.h"
#include "playback/state/editing/commands/SequenceCommands.h"
#include "playback/state/editing/commands/SubActorCommands.h"
#include "playback/state/editing/commands/WorldActorCommands.h"

namespace playback::state::editing::command {

std::unique_ptr<model::IEditCommand> CommandFactory::createAddCameraSequence() {
    return std::make_unique<AddCameraSequence>();
}
std::unique_ptr<model::IEditCommand> CommandFactory::createDeleteCameraSequence() {
    return std::make_unique<DeleteCameraSequence>();
}
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
std::unique_ptr<model::IEditCommand> CommandFactory::createAddCameraKeyframe(
    const std::string&                   id,
    int                                  tick,
    std::optional<model::CameraKeyframe> captured
) {
    return std::make_unique<AddKeyframe>(id, tick, std::move(captured));
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createMoveCameraKeyframe(const std::string& id, int fromTick, int toTick) {
    return std::make_unique<MoveKeyframe>(id, fromTick, toTick);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createDeleteCameraKeyframe(const std::string& id, int tick) {
    return std::make_unique<DeleteKeyframe>(id, tick);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSetKeyframeInterpolation(
    const std::string&             id,
    int                            tick,
    model::CameraInterpolationType interpolation
) {
    return std::make_unique<SetKeyframeInterpolation>(id, tick, interpolation);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSetCameraEnabled(const std::string& id, bool enabled) {
    return std::make_unique<SetCameraTrackState>(id, SetCameraTrackState::Property::Enabled, enabled);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetSubActorDetails(const std::string& id, model::AgentDetails details) {
    return std::make_unique<SetSubActorDetails>(id, std::move(details));
}

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

std::unique_ptr<model::IEditCommand> CommandFactory::createAddTransition(
    const std::string&    fromClipId,
    const std::string&    toClipId,
    model::TransitionKind kind,
    int                   durationTicks
) {
    return std::make_unique<AddTransitionCommand>(fromClipId, toClipId, kind, durationTicks);
}

std::unique_ptr<model::IEditCommand> CommandFactory::createAddTrack(model::TrackKind kind, const std::string& name) {
    (void)kind;
    (void)name;
    return nullptr;
}

std::unique_ptr<model::IEditCommand> CommandFactory::createRemoveTrack(const std::string& trackId) {
    (void)trackId;
    return nullptr;
}

} // namespace playback::state::editing::command
