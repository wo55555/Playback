#pragma once

#include "playback/editor/editing/models/CameraEntity.h"
#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/editor/editing/models/IEditCommand.h"
#include "playback/editor/editing/models/Track.h"

#include <memory>
#include <string>

namespace playback::editor::editing::command {

class CommandFactory {
public:
    static std::unique_ptr<model::IEditCommand> createSplitSequence(int atTick);
    static std::unique_ptr<model::IEditCommand> createTrimSequence(const std::string& id, int start, int end);
    static std::unique_ptr<model::IEditCommand> createDeleteSequenceSegment(const std::string& id);
    static std::unique_ptr<model::IEditCommand>
    createBindSequenceToCamera(const std::string& id, const std::string& cameraId);
    static std::unique_ptr<model::IEditCommand> createSplitWorldActor(int tick);
    static std::unique_ptr<model::IEditCommand> createTrimWorldActor(const std::string& id, int start, int end);
    static std::unique_ptr<model::IEditCommand> createSetWorldActorSpeed(const std::string& id, float speed);
    static std::unique_ptr<model::IEditCommand> createRippleDeleteWorldActorSegment(const std::string& id);
    static std::unique_ptr<model::IEditCommand> createAddFreeCamera(const std::string& name);
    static std::unique_ptr<model::IEditCommand> createDeleteCamera(const std::string& id);
    static std::unique_ptr<model::IEditCommand>
    createCreateBindingCamera(const std::string& id, const std::string& name);
    static std::unique_ptr<model::IEditCommand> createUnbindCamera(const std::string& id);
    static std::unique_ptr<model::IEditCommand> createAddCameraKeyframe(const std::string& id, int tick);
    static std::unique_ptr<model::IEditCommand>
    createMoveCameraKeyframe(const std::string& id, const std::string& keyframeId, int tick);
    static std::unique_ptr<model::IEditCommand>
    createDeleteCameraKeyframe(const std::string& id, const std::string& keyframeId);
    static std::unique_ptr<model::IEditCommand> createSetCameraKind(const std::string& id, model::CameraKind kind);

    static std::unique_ptr<model::IEditCommand>
    createSplitClip(const std::string& trackId, const std::string& clipId, int atTick);

    static std::unique_ptr<model::IEditCommand> createRemoveClip(const std::string& trackId, const std::string& clipId);

    static std::unique_ptr<model::IEditCommand>
    createTrimClip(const std::string& trackId, const std::string& clipId, int newInTick, int newOutTick);

    static std::unique_ptr<model::IEditCommand>
    createMoveClip(const std::string& trackId, const std::string& clipId, int newTrackTick);

    static std::unique_ptr<model::IEditCommand> createAddTransition(
        const std::string&    fromClipId,
        const std::string&    toClipId,
        model::TransitionKind kind,
        int                   durationTicks
    );

    static std::unique_ptr<model::IEditCommand> createAddTrack(model::TrackKind kind, const std::string& name);

    static std::unique_ptr<model::IEditCommand> createRemoveTrack(const std::string& trackId);
};

} // namespace playback::editor::editing::command
