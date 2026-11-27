#pragma once

#include "playback/state/editing/models/CameraEntity.h"
#include "playback/state/editing/models/IEditCommand.h"
#include "playback/state/editing/models/SelectionModel.h"
#include "playback/state/editing/models/Track.h"

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
    static std::unique_ptr<model::IEditCommand> createSetCameraKeyframeMode(
        const std::string&        id,
        int                       tick,
        model::CameraKeyframeMode mode,
        const std::string&        defaultSubject = {}
    );
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
    static std::unique_ptr<model::IEditCommand> createSetKeyframesMode(
        std::vector<model::KeyframeRef> refs,
        model::CameraKeyframeMode       mode,
        const std::string&              defaultSubject = {}
    );
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
    createAddClip(const std::string& cameraId, int inTick, int outTick, const std::string& name);
    static std::unique_ptr<model::IEditCommand>
    createSplitClip(const std::string& clipId, int splitTick, const std::string& newClipName = {});
    static std::unique_ptr<model::IEditCommand>
    createMoveAndTrimClip(const std::string& clipId, int newInTick, int newOutTick);
    static std::unique_ptr<model::IEditCommand> createRenameClip(const std::string& clipId, const std::string& name);
    static std::unique_ptr<model::IEditCommand> createSetClipLocked(const std::string& clipId, bool locked);
    static std::unique_ptr<model::IEditCommand>
    createSetClipExportEnabled(const std::string& clipId, bool exportEnabled);
    static std::unique_ptr<model::IEditCommand>
    createSetAllClipsExportEnabled(bool exportEnabled, const std::string& cameraId = {});
    static std::unique_ptr<model::IEditCommand>
    createResetClipsToFullRange(const std::string& cameraId = {}, const std::string& defaultClipName = {});
    static std::unique_ptr<model::IEditCommand> createDeleteClip(const std::string& clipId);
    static std::unique_ptr<model::IEditCommand> createHealClipCuts(
        std::vector<model::SelectedClipCut> cuts,
        std::vector<model::KeyframeRef>     keyframesToDelete = {}
    );

    static std::unique_ptr<model::IEditCommand> createAddScene(const std::string& name = {});
    static std::unique_ptr<model::IEditCommand>
    createDuplicateScene(const std::string& sceneId, const std::string& copyName = {});
    static std::unique_ptr<model::IEditCommand> createRenameScene(const std::string& sceneId, const std::string& name);
    static std::unique_ptr<model::IEditCommand> createMoveScene(const std::string& sceneId, int targetIndex);
    static std::unique_ptr<model::IEditCommand> createDeleteScene(const std::string& sceneId);
    static std::unique_ptr<model::IEditCommand> createSwitchScene(const std::string& sceneId);
    static std::unique_ptr<model::IEditCommand> createSetSceneExportRange(int inTick, int outTick);

    static std::unique_ptr<model::IEditCommand> createAddSpeedTrack();
    static std::unique_ptr<model::IEditCommand> createRemoveSpeedTrack();
    static std::unique_ptr<model::IEditCommand> createAddSpeedKeyframe(int tick, float speed = 1.0f);
    static std::unique_ptr<model::IEditCommand> createMoveSpeedKeyframe(int fromTick, int toTick);
    static std::unique_ptr<model::IEditCommand> createDeleteSpeedKeyframe(int tick);
    static std::unique_ptr<model::IEditCommand> createSetSpeedKeyframeValue(int tick, float speed);
    static std::unique_ptr<model::IEditCommand>
    createSetSpeedKeyframeInterpolation(int tick, model::CameraInterpolationType interpolation);
    static std::unique_ptr<model::IEditCommand>
    createSetSpeedKeyframeBezierControls(int tick, model::Vec2 ctrl1, model::Vec2 ctrl2);
    static std::unique_ptr<model::IEditCommand> createSetSpeedTrackEnabled(bool enabled);
    static std::unique_ptr<model::IEditCommand> createSetSpeedTrackLocked(bool locked);
    static std::unique_ptr<model::IEditCommand> createClearSpeedKeyframes();

    static std::unique_ptr<model::IEditCommand> createAddFreezeTrack();
    static std::unique_ptr<model::IEditCommand> createRemoveFreezeTrack();
    static std::unique_ptr<model::IEditCommand>
    createAddFreezeKeyframe(int tick, bool frozen = true, int frozenDelay = 0);
    static std::unique_ptr<model::IEditCommand> createMoveFreezeKeyframe(int fromTick, int toTick);
    static std::unique_ptr<model::IEditCommand> createDeleteFreezeKeyframe(int tick);
    static std::unique_ptr<model::IEditCommand> createSetFreezeKeyframe(int tick, bool frozen, int frozenDelay);
    static std::unique_ptr<model::IEditCommand> createSetFreezeTrackEnabled(bool enabled);
    static std::unique_ptr<model::IEditCommand> createSetFreezeTrackLocked(bool locked);
    static std::unique_ptr<model::IEditCommand> createClearFreezeKeyframes();

    static std::unique_ptr<model::IEditCommand> createAddTimelapseTrack();
    static std::unique_ptr<model::IEditCommand> createRemoveTimelapseTrack();
    static std::unique_ptr<model::IEditCommand> createAddTimelapseKeyframe(int tick, int outputTicks = 0);
    static std::unique_ptr<model::IEditCommand> createMoveTimelapseKeyframe(int fromTick, int toTick);
    static std::unique_ptr<model::IEditCommand> createDeleteTimelapseKeyframe(int tick);
    static std::unique_ptr<model::IEditCommand> createSetTimelapseKeyframeOutputTicks(int tick, int outputTicks);
    static std::unique_ptr<model::IEditCommand> createSetTimelapseTrackEnabled(bool enabled);
    static std::unique_ptr<model::IEditCommand> createSetTimelapseTrackLocked(bool locked);
    static std::unique_ptr<model::IEditCommand> createClearTimelapseKeyframes();

    static std::unique_ptr<model::IEditCommand> createAddTimeOfDayTrack();
    static std::unique_ptr<model::IEditCommand> createRemoveTimeOfDayTrack();
    static std::unique_ptr<model::IEditCommand> createSetTimeOfDayTrackEnabled(bool enabled);
    static std::unique_ptr<model::IEditCommand> createSetTimeOfDayTrackLocked(bool locked);
    static std::unique_ptr<model::IEditCommand> createAddTimeOfDayKeyframe(
        int                            tick,
        int                            timeOfDay         = 6000,
        model::CameraInterpolationType interpolationType = model::CameraInterpolationType::Linear
    );
    static std::unique_ptr<model::IEditCommand> createRemoveTimeOfDayKeyframe(int tick);
    static std::unique_ptr<model::IEditCommand> createRemoveTimeOfDayKeyframes(std::vector<int> ticks);
    static std::unique_ptr<model::IEditCommand> createMoveTimeOfDayKeyframe(int fromTick, int toTick);
    static std::unique_ptr<model::IEditCommand> createMoveTimeOfDayKeyframes(std::vector<int> ticks, int tickDelta);
    static std::unique_ptr<model::IEditCommand> createSetTimeOfDayKeyframeValue(int tick, int timeOfDay);
    static std::unique_ptr<model::IEditCommand>
    createSetTimeOfDayKeyframeInterpolation(int tick, model::CameraInterpolationType interpolation);
    static std::unique_ptr<model::IEditCommand>
    createSetTimeOfDayKeyframeBezier(int tick, model::Vec2 ctrl1, model::Vec2 ctrl2);
    static std::unique_ptr<model::IEditCommand>
    createSetTimeOfDayKeyframesInterpolation(std::vector<int> ticks, model::CameraInterpolationType interpolation);
    static std::unique_ptr<model::IEditCommand> createClearTimeOfDayKeyframes();
    static std::unique_ptr<model::IEditCommand> createSetSceneDaytimeOverride(bool enabled, int ticks);
    static std::unique_ptr<model::IEditCommand> createSetSceneWeatherOverride(model::WeatherOverride weather);

    static std::unique_ptr<model::IEditCommand> createAddWeatherTrack();
    static std::unique_ptr<model::IEditCommand> createRemoveWeatherTrack();
    static std::unique_ptr<model::IEditCommand> createSetWeatherTrackEnabled(bool enabled);
    static std::unique_ptr<model::IEditCommand> createSetWeatherTrackLocked(bool locked);
    static std::unique_ptr<model::IEditCommand>
    createAddWeatherKeyframe(int tick, model::WeatherOverride weather = model::WeatherOverride::Clear);
    static std::unique_ptr<model::IEditCommand> createRemoveWeatherKeyframe(int tick);
    static std::unique_ptr<model::IEditCommand> createRemoveWeatherKeyframes(std::vector<int> ticks);
    static std::unique_ptr<model::IEditCommand> createMoveWeatherKeyframe(int fromTick, int toTick);
    static std::unique_ptr<model::IEditCommand> createMoveWeatherKeyframes(std::vector<int> ticks, int tickDelta);
    static std::unique_ptr<model::IEditCommand> createSetWeatherKeyframeValue(int tick, model::WeatherOverride weather);
    static std::unique_ptr<model::IEditCommand>
    createSetWeatherKeyframesValue(std::vector<int> ticks, model::WeatherOverride weather);

    static std::unique_ptr<model::IEditCommand> createAddAudioTrack(const std::string& name = {});
    static std::unique_ptr<model::IEditCommand> createRemoveAudioTrack(const std::string& trackId);
    static std::unique_ptr<model::IEditCommand>
    createRenameAudioTrack(const std::string& trackId, const std::string& name);
    static std::unique_ptr<model::IEditCommand> createSetAudioTrackEnabled(const std::string& trackId, bool enabled);
    static std::unique_ptr<model::IEditCommand> createSetAudioTrackMuted(const std::string& trackId, bool muted);
    static std::unique_ptr<model::IEditCommand> createSetAudioTrackLocked(const std::string& trackId, bool locked);
    static std::unique_ptr<model::IEditCommand> createSetAudioTrackVolume(const std::string& trackId, float volume);
    static std::unique_ptr<model::IEditCommand> createAddAudioClip(
        const std::string& trackId,
        const std::string& filePath,
        int                startTick,
        const std::string& name = {}
    );
    static std::unique_ptr<model::IEditCommand>
    createRemoveAudioClip(const std::string& trackId, const std::string& clipId);
    static std::unique_ptr<model::IEditCommand>
    createMoveAudioClip(const std::string& trackId, const std::string& clipId, int startTick);
    static std::unique_ptr<model::IEditCommand> createSetAudioClipProps(
        const std::string& trackId,
        const std::string& clipId,
        const std::string& filePath,
        double             trimStartSeconds,
        double             durationSeconds,
        float              volume,
        bool               muted
    );
};

} // namespace playback::state::editing::command
