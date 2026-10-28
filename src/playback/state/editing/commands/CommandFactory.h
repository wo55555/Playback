#pragma once

#include "playback/state/editing/models/CameraEntity.h"
#include "playback/state/editing/models/IEditCommand.h"
#include "playback/state/editing/models/SubActor.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace playback::state::editing::command {

class CommandFactory {
public:
    static std::unique_ptr<model::IEditCommand> createAddCameraSequence();
    static std::unique_ptr<model::IEditCommand> createDeleteCameraSequence();
    static std::unique_ptr<model::IEditCommand> createSplitSequence(int atTick);
    static std::unique_ptr<model::IEditCommand> createTrimSequence(const std::string& id, int start, int end);
    static std::unique_ptr<model::IEditCommand> createDeleteSequenceSegment(const std::string& id);
    static std::unique_ptr<model::IEditCommand> createMergeSequenceSegment(const std::string& id, bool withNext);
    static std::unique_ptr<model::IEditCommand>
    createBindSequenceToCamera(const std::string& id, const std::string& cameraId);
    static std::unique_ptr<model::IEditCommand> createSplitWorldActor(int tick);
    static std::unique_ptr<model::IEditCommand> createTrimWorldActor(const std::string& id, int start, int end);

    static std::unique_ptr<model::IEditCommand> createRippleDeleteWorldActorSegment(const std::string& id);
    static std::unique_ptr<model::IEditCommand> createAddFreeCamera(const std::string& name);
    static std::unique_ptr<model::IEditCommand> createDeleteCamera(const std::string& id);
    static std::unique_ptr<model::IEditCommand>
    createCreateBindingCamera(const std::string& id, const std::string& name);
    static std::unique_ptr<model::IEditCommand> createUnbindCamera(const std::string& id);
    static std::unique_ptr<model::IEditCommand> createAddCameraKeyframe(const std::string& id, int tick);
    static std::unique_ptr<model::IEditCommand>
    createAddCameraKeyframe(const std::string& id, int tick, std::optional<model::CameraKeyframe> captured);
    static std::unique_ptr<model::IEditCommand>
    createMoveCameraKeyframe(const std::string& id, int fromTick, int toTick);
    static std::unique_ptr<model::IEditCommand> createDeleteCameraKeyframe(const std::string& id, int tick);
    static std::unique_ptr<model::IEditCommand>
    createSetKeyframeInterpolation(const std::string& id, int tick, model::CameraInterpolationType interpolation);
    static std::unique_ptr<model::IEditCommand>
    createSetCameraKeyframePosition(const std::string& id, int tick, model::Vec3 position);
    static std::unique_ptr<model::IEditCommand>
    createSetCameraKeyframeRotation(const std::string& id, int tick, model::Vec3 rotation);
    static std::unique_ptr<model::IEditCommand> createSetCameraKeyframeFov(const std::string& id, int tick, float fov);
    static std::unique_ptr<model::IEditCommand> createSetCameraEnabled(const std::string& id, bool enabled);
    static std::unique_ptr<model::IEditCommand> createSetCameraLocked(const std::string& id, bool locked);
    static std::unique_ptr<model::IEditCommand> createRenameCamera(const std::string& id, const std::string& name);
    static std::unique_ptr<model::IEditCommand> createClearCameraKeyframes(const std::string& id);
    static std::unique_ptr<model::IEditCommand>
    createSetSubActorDetails(const std::string& id, model::AgentDetails details);

    static std::unique_ptr<model::IEditCommand> createMoveCameraKeyframes(std::vector<model::KeyframeRef> refs, int delta);
    static std::unique_ptr<model::IEditCommand> createDeleteCameraKeyframes(std::vector<model::KeyframeRef> refs);
    static std::unique_ptr<model::IEditCommand>
    createSetKeyframesInterpolation(std::vector<model::KeyframeRef> refs, model::CameraInterpolationType interpolation);
    static std::unique_ptr<model::IEditCommand> createAddCameraKeyframes(std::vector<model::KeyframeRef> refs);

    static std::unique_ptr<model::IEditCommand>
    createAddMarker(const std::string& id, int tick, const std::string& label);
    static std::unique_ptr<model::IEditCommand> createDeleteMarker(const std::string& id);
    static std::unique_ptr<model::IEditCommand> createMoveMarker(const std::string& id, int tick);
    static std::unique_ptr<model::IEditCommand> createRenameMarker(const std::string& id, const std::string& label);
};

} // namespace playback::state::editing::command
