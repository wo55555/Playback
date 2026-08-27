#include "CommandFactory.h"

#include "playback/state/editing/commands/CameraCommands.h"
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
    return std::make_unique<AddKeyframe>(id, tick, captured);
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
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetCameraKeyframePosition(const std::string& id, int tick, model::Vec3 position) {
    return std::make_unique<SetCameraKeyframePosition>(id, tick, position);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetCameraKeyframeRotation(const std::string& id, int tick, model::Vec3 rotation) {
    return std::make_unique<SetCameraKeyframeRotation>(id, tick, rotation);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetCameraKeyframeFov(const std::string& id, int tick, float fov) {
    return std::make_unique<SetCameraKeyframeFov>(id, tick, fov);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSetCameraEnabled(const std::string& id, bool enabled) {
    return std::make_unique<SetCameraTrackState>(id, SetCameraTrackState::Property::Enabled, enabled);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetSubActorDetails(const std::string& id, model::AgentDetails details) {
    return std::make_unique<SetSubActorDetails>(id, std::move(details));
}

} // namespace playback::state::editing::command
