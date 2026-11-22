#include "CommandFactory.h"

#include "playback/state/editing/commands/CameraCommands.h"
#include "playback/state/editing/commands/ClipCommands.h"
#include "playback/state/editing/commands/MarkerCommands.h"
#include "playback/state/editing/commands/SpeedCommands.h"

namespace playback::state::editing::command {

std::unique_ptr<model::IEditCommand> CommandFactory::createAddFreeCamera(const std::string& name) {
    return std::make_unique<AddFreeCamera>(name);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createDeleteCamera(const std::string& id) {
    return std::make_unique<DeleteCamera>(id);
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
std::unique_ptr<model::IEditCommand> CommandFactory::createSetCameraKeyframeBezierControls(
    const std::string& id,
    int                tick,
    model::Vec2        ctrl1,
    model::Vec2        ctrl2
) {
    return std::make_unique<SetCameraKeyframeBezierControls>(id, tick, ctrl1, ctrl2);
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
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetCameraKeyframeSubject(const std::string& id, int tick, const std::string& subject) {
    return std::make_unique<SetCameraKeyframeSubject>(id, tick, subject);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetCameraKeyframeMode(const std::string& id, int tick, model::CameraKeyframeMode mode) {
    return std::make_unique<SetCameraKeyframeMode>(id, tick, mode);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSetCameraEnabled(const std::string& id, bool enabled) {
    return std::make_unique<SetCameraTrackState>(id, SetCameraTrackState::Property::Enabled, enabled);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSetCameraLocked(const std::string& id, bool locked) {
    return std::make_unique<SetCameraTrackState>(id, SetCameraTrackState::Property::Locked, locked);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetCameraShake(const std::string& id, std::optional<model::CameraShake> shake) {
    return std::make_unique<SetCameraShake>(id, shake);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createRenameCamera(const std::string& id, const std::string& name) {
    return std::make_unique<RenameCamera>(id, name);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createClearCameraKeyframes(const std::string& id) {
    return std::make_unique<ClearCameraKeyframes>(id);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createMoveCamera(const std::string& id, int targetIndex) {
    return std::make_unique<MoveCamera>(id, targetIndex);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createMoveCameraKeyframes(std::vector<model::KeyframeRef> refs, int delta) {
    return std::make_unique<MoveKeyframes>(std::move(refs), delta);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createDeleteCameraKeyframes(std::vector<model::KeyframeRef> refs) {
    return std::make_unique<DeleteKeyframes>(std::move(refs));
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSetKeyframesInterpolation(
    std::vector<model::KeyframeRef> refs,
    model::CameraInterpolationType  interpolation
) {
    return std::make_unique<SetKeyframesInterpolation>(std::move(refs), interpolation);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetKeyframesMode(std::vector<model::KeyframeRef> refs, model::CameraKeyframeMode mode) {
    return std::make_unique<SetKeyframesMode>(std::move(refs), mode);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetKeyframesSubject(std::vector<model::KeyframeRef> refs, const std::string& subject) {
    return std::make_unique<SetKeyframesSubject>(std::move(refs), subject);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createAddCameraKeyframes(std::vector<model::KeyframeRef> refs) {
    return std::make_unique<AddKeyframes>(std::move(refs));
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetCameraKeyframeIgnoreClipSpeed(const std::string& id, int tick, bool ignoreClipSpeed) {
    return std::make_unique<SetCameraKeyframeIgnoreClipSpeed>(id, tick, ignoreClipSpeed);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetKeyframesIgnoreClipSpeed(std::vector<model::KeyframeRef> refs, bool ignoreClipSpeed) {
    return std::make_unique<SetKeyframesIgnoreClipSpeed>(std::move(refs), ignoreClipSpeed);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSetCameraKeyframeThirdPersonSettings(
    const std::string&    id,
    int                   tick,
    model::CameraKeyframe settings
) {
    return std::make_unique<SetCameraKeyframeThirdPersonSettings>(id, tick, std::move(settings));
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetKeyframesThirdPersonCustom(std::vector<model::KeyframeRef> refs, bool custom) {
    return std::make_unique<SetKeyframesThirdPersonCustom>(std::move(refs), custom);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSetCameraKeyframeLookAt(
    const std::string& id,
    int                tick,
    bool               enabled,
    const std::string& target,
    float              offsetY
) {
    return std::make_unique<SetCameraKeyframeLookAt>(id, tick, enabled, target, offsetY);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSetKeyframesLookAt(
    std::vector<model::KeyframeRef> refs,
    bool                            enabled,
    const std::string&              target,
    std::optional<float>            offsetY
) {
    return std::make_unique<SetKeyframesLookAt>(std::move(refs), enabled, target, offsetY);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createUpdateCameraKeyframePose(const std::string& id, int tick, model::CameraKeyframe pose) {
    return std::make_unique<UpdateCameraKeyframePose>(id, tick, std::move(pose));
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createAddMarker(const std::string& id, int tick, const std::string& label) {
    return std::make_unique<AddMarker>(id, tick, label);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createDeleteMarker(const std::string& id) {
    return std::make_unique<DeleteMarker>(id);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createMoveMarker(const std::string& id, int tick) {
    return std::make_unique<MoveMarker>(id, tick);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createRenameMarker(const std::string& id, const std::string& label) {
    return std::make_unique<RenameMarker>(id, label);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createAddClip(const std::string& cameraId, int inTick, int outTick, const std::string& name) {
    return std::make_unique<AddClip>(cameraId, inTick, outTick, name);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createSplitClip(const std::string& clipId, int splitTick, const std::string& newClipName) {
    return std::make_unique<SplitClip>(clipId, splitTick, newClipName);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createMoveAndTrimClip(const std::string& clipId, int newInTick, int newOutTick) {
    return std::make_unique<MoveAndTrimClip>(clipId, newInTick, newOutTick);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createRenameClip(const std::string& clipId, const std::string& name) {
    return std::make_unique<RenameClip>(clipId, name);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSetClipMuted(const std::string& clipId, bool muted) {
    return std::make_unique<SetClipMuted>(clipId, muted);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSetClipLocked(const std::string& clipId, bool locked) {
    return std::make_unique<SetClipLocked>(clipId, locked);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetClipExportEnabled(const std::string& clipId, bool exportEnabled) {
    return std::make_unique<SetClipExportEnabled>(clipId, exportEnabled);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetAllClipsExportEnabled(bool exportEnabled, const std::string& cameraId) {
    return std::make_unique<SetAllClipsExportEnabled>(exportEnabled, cameraId);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createResetClipsToFullRange(const std::string& cameraId, const std::string& defaultClipName) {
    return std::make_unique<ResetClipsToFullRange>(cameraId, defaultClipName);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createDeleteClip(const std::string& clipId) {
    return std::make_unique<DeleteClip>(clipId);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createAddScene(const std::string& name) {
    return std::make_unique<AddScene>(name);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createDuplicateScene(const std::string& sceneId, const std::string& copyName) {
    return std::make_unique<DuplicateScene>(sceneId, copyName);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createRenameScene(const std::string& sceneId, const std::string& name) {
    return std::make_unique<RenameScene>(sceneId, name);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createMoveScene(const std::string& sceneId, int targetIndex) {
    return std::make_unique<MoveScene>(sceneId, targetIndex);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createDeleteScene(const std::string& sceneId) {
    return std::make_unique<DeleteScene>(sceneId);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSwitchScene(const std::string& sceneId) {
    return std::make_unique<SwitchScene>(sceneId);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSetSceneExportRange(int inTick, int outTick) {
    return std::make_unique<SetSceneExportRange>(inTick, outTick);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createAddSpeedTrack() { return std::make_unique<AddSpeedTrack>(); }
std::unique_ptr<model::IEditCommand> CommandFactory::createRemoveSpeedTrack() {
    return std::make_unique<RemoveSpeedTrack>();
}
std::unique_ptr<model::IEditCommand> CommandFactory::createAddSpeedKeyframe(int tick, float speed) {
    return std::make_unique<AddSpeedKeyframe>(tick, speed);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createMoveSpeedKeyframe(int fromTick, int toTick) {
    return std::make_unique<MoveSpeedKeyframe>(fromTick, toTick);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createDeleteSpeedKeyframe(int tick) {
    return std::make_unique<DeleteSpeedKeyframe>(tick);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSetSpeedKeyframeValue(int tick, float speed) {
    return std::make_unique<SetSpeedKeyframeValue>(tick, speed);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetSpeedKeyframeInterpolation(int tick, model::CameraInterpolationType interpolation) {
    return std::make_unique<SetSpeedKeyframeInterpolation>(tick, interpolation);
}
std::unique_ptr<model::IEditCommand>
CommandFactory::createSetSpeedKeyframeBezierControls(int tick, model::Vec2 ctrl1, model::Vec2 ctrl2) {
    return std::make_unique<SetSpeedKeyframeBezierControls>(tick, ctrl1, ctrl2);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSetSpeedTrackEnabled(bool enabled) {
    return std::make_unique<SetSpeedTrackState>(SetSpeedTrackState::Property::Enabled, enabled);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createSetSpeedTrackLocked(bool locked) {
    return std::make_unique<SetSpeedTrackState>(SetSpeedTrackState::Property::Locked, locked);
}
std::unique_ptr<model::IEditCommand> CommandFactory::createClearSpeedKeyframes() {
    return std::make_unique<ClearSpeedKeyframes>();
}

} // namespace playback::state::editing::command
