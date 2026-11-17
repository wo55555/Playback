#pragma once

#include "playback/state/editing/models/CameraEntity.h"
#include "playback/state/editing/models/IEditCommand.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace playback::state::editing::command {

class CommandFactory {
public:
    static std::unique_ptr<model::IEditCommand> createAddFreeCamera(const std::string& name);
    static std::unique_ptr<model::IEditCommand> createDeleteCamera(const std::string& id);
    static std::unique_ptr<model::IEditCommand> createAddCameraKeyframe(const std::string& id, int tick);
    static std::unique_ptr<model::IEditCommand>
    createAddCameraKeyframe(const std::string& id, int tick, std::optional<model::CameraKeyframe> captured);
    static std::unique_ptr<model::IEditCommand>
    createMoveCameraKeyframe(const std::string& id, int fromTick, int toTick);
    static std::unique_ptr<model::IEditCommand> createDeleteCameraKeyframe(const std::string& id, int tick);
    static std::unique_ptr<model::IEditCommand>
    createSetKeyframeInterpolation(const std::string& id, int tick, model::CameraInterpolationType interpolation);
    static std::unique_ptr<model::IEditCommand>
    createSetCameraKeyframeBezierControls(const std::string& id, int tick, model::Vec2 ctrl1, model::Vec2 ctrl2);
    static std::unique_ptr<model::IEditCommand>
    createSetCameraKeyframePosition(const std::string& id, int tick, model::Vec3 position);
    static std::unique_ptr<model::IEditCommand>
    createSetCameraKeyframeRotation(const std::string& id, int tick, model::Vec3 rotation);
    static std::unique_ptr<model::IEditCommand> createSetCameraKeyframeFov(const std::string& id, int tick, float fov);
    static std::unique_ptr<model::IEditCommand>
    createSetCameraKeyframeSubject(const std::string& id, int tick, const std::string& subject);
    static std::unique_ptr<model::IEditCommand>
    createSetCameraKeyframeMode(const std::string& id, int tick, model::CameraKeyframeMode mode);
    static std::unique_ptr<model::IEditCommand> createSetCameraEnabled(const std::string& id, bool enabled);
    static std::unique_ptr<model::IEditCommand> createSetCameraLocked(const std::string& id, bool locked);
    static std::unique_ptr<model::IEditCommand>
    createSetCameraShake(const std::string& id, std::optional<model::CameraShake> shake);
    static std::unique_ptr<model::IEditCommand> createRenameCamera(const std::string& id, const std::string& name);
    static std::unique_ptr<model::IEditCommand> createClearCameraKeyframes(const std::string& id);
    static std::unique_ptr<model::IEditCommand> createMoveCamera(const std::string& id, int targetIndex);

    static std::unique_ptr<model::IEditCommand> createMoveCameraKeyframes(std::vector<model::KeyframeRef> refs, int delta);
    static std::unique_ptr<model::IEditCommand> createDeleteCameraKeyframes(std::vector<model::KeyframeRef> refs);
    static std::unique_ptr<model::IEditCommand>
    createSetKeyframesInterpolation(std::vector<model::KeyframeRef> refs, model::CameraInterpolationType interpolation);
    static std::unique_ptr<model::IEditCommand>
    createSetKeyframesMode(std::vector<model::KeyframeRef> refs, model::CameraKeyframeMode mode);
    static std::unique_ptr<model::IEditCommand>
    createSetKeyframesSubject(std::vector<model::KeyframeRef> refs, const std::string& subject);
    static std::unique_ptr<model::IEditCommand> createAddCameraKeyframes(std::vector<model::KeyframeRef> refs);

    static std::unique_ptr<model::IEditCommand>
    createAddMarker(const std::string& id, int tick, const std::string& label);
    static std::unique_ptr<model::IEditCommand> createDeleteMarker(const std::string& id);
    static std::unique_ptr<model::IEditCommand> createMoveMarker(const std::string& id, int tick);
    static std::unique_ptr<model::IEditCommand> createRenameMarker(const std::string& id, const std::string& label);
};

} // namespace playback::state::editing::command
