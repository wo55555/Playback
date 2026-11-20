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
    createSetCameraKeyframeIgnoreClipSpeed(const std::string& id, int tick, bool ignoreClipSpeed);
    static std::unique_ptr<model::IEditCommand>
    createSetKeyframesIgnoreClipSpeed(std::vector<model::KeyframeRef> refs, bool ignoreClipSpeed);
    static std::unique_ptr<model::IEditCommand>
    createSetCameraKeyframeThirdPersonSettings(const std::string& id, int tick, model::CameraKeyframe settings);
    static std::unique_ptr<model::IEditCommand>
    createSetKeyframesThirdPersonCustom(std::vector<model::KeyframeRef> refs, bool custom);
    static std::unique_ptr<model::IEditCommand>
    createSetCameraKeyframeLookAt(const std::string& id, int tick, bool enabled, const std::string& target, float offsetY);
    static std::unique_ptr<model::IEditCommand> createSetKeyframesLookAt(
        std::vector<model::KeyframeRef> refs,
        bool                            enabled,
        const std::string&              target,
        std::optional<float>            offsetY
    );
    static std::unique_ptr<model::IEditCommand>
    createUpdateCameraKeyframePose(const std::string& id, int tick, model::CameraKeyframe pose);

    static std::unique_ptr<model::IEditCommand>
    createAddMarker(const std::string& id, int tick, const std::string& label);
    static std::unique_ptr<model::IEditCommand> createDeleteMarker(const std::string& id);
    static std::unique_ptr<model::IEditCommand> createMoveMarker(const std::string& id, int tick);
    static std::unique_ptr<model::IEditCommand> createRenameMarker(const std::string& id, const std::string& label);

    static std::unique_ptr<model::IEditCommand>
    createAddClip(const std::string& cameraId, int inTick, int outTick, const std::string& name, float speed = 1.0f);
    static std::unique_ptr<model::IEditCommand>
    createSplitClip(const std::string& clipId, int splitTick, const std::string& newClipName = {});
    static std::unique_ptr<model::IEditCommand>
    createMoveAndTrimClip(const std::string& clipId, int newInTick, int newOutTick);
    static std::unique_ptr<model::IEditCommand> createSetClipSpeed(const std::string& clipId, float speed);
    static std::unique_ptr<model::IEditCommand> createRenameClip(const std::string& clipId, const std::string& name);
    static std::unique_ptr<model::IEditCommand> createSetClipMuted(const std::string& clipId, bool muted);
    static std::unique_ptr<model::IEditCommand> createSetClipLocked(const std::string& clipId, bool locked);
    static std::unique_ptr<model::IEditCommand>
    createSetClipExportEnabled(const std::string& clipId, bool exportEnabled);
    static std::unique_ptr<model::IEditCommand>
    createSetAllClipsExportEnabled(bool exportEnabled, const std::string& cameraId = {});
    static std::unique_ptr<model::IEditCommand>
    createResetClipsToFullRange(const std::string& cameraId = {}, const std::string& defaultClipName = {});
    static std::unique_ptr<model::IEditCommand> createDeleteClip(const std::string& clipId);

    static std::unique_ptr<model::IEditCommand> createAddScene(const std::string& name = {});
    static std::unique_ptr<model::IEditCommand>
    createDuplicateScene(const std::string& sceneId, const std::string& copyName = {});
    static std::unique_ptr<model::IEditCommand> createRenameScene(const std::string& sceneId, const std::string& name);
    static std::unique_ptr<model::IEditCommand> createMoveScene(const std::string& sceneId, int targetIndex);
    static std::unique_ptr<model::IEditCommand> createDeleteScene(const std::string& sceneId);
    static std::unique_ptr<model::IEditCommand> createSwitchScene(const std::string& sceneId);
    static std::unique_ptr<model::IEditCommand> createSetSceneExportRange(int inTick, int outTick);
};

} // namespace playback::state::editing::command
