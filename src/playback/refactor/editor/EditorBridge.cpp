#include "EditorBridge.h"

#include "playback/editor/context/EditorAction.h"
#include "playback/editor/context/EditorContext.h"
#include "playback/functions/replay/ReplaySession.h"

#include "playback/refactor/video-editing/EditingCommands.h"
#include "CommandFactory.h"

#include <algorithm>

namespace playback::refactor::editor {

using namespace playback::editor;

// ===== Singleton =====

EditorBridge& EditorBridge::getInstance() {
    static EditorBridge instance;
    return instance;
}

// ===== Lifecycle =====

void EditorBridge::initialize(EditorContext* context) {
    mContext = context;
    mCommandStack.clear();
    mPendingActions.clear();
}

void EditorBridge::shutdown() {
    mContext = nullptr;
    mCommandStack.clear();
    mPendingActions.clear();
}

// ===== Frame sync =====

void EditorBridge::syncState(EditorStateExt& outState) {
    if (!mContext) return;

    auto const oldState = mContext->snapshot();

    // Sync basic playback state
    outState.currentTick = oldState.currentTick;
    outState.totalTicks  = oldState.totalTicks;
    outState.playing     = !oldState.paused;
    outState.playbackSpeed = oldState.playbackSpeed;
    outState.fps         = 20.0f; // Minecraft ticks per second
}

void EditorBridge::commitState() {
    if (!mContext || mPendingActions.empty()) return;

    for (auto& action : mPendingActions) {
        mContext->submit(action);
    }
    mPendingActions.clear();
}

// ===== Playback control =====

void EditorBridge::playPause() {
    submitAction({EditorActionType::TogglePause});
}

void EditorBridge::seek(int tick) {
    submitAction({EditorActionType::Seek, tick});
}

void EditorBridge::skipToStart() {
    submitAction({EditorActionType::SkipToStart});
}

void EditorBridge::skipToEnd() {
    submitAction({EditorActionType::SkipToEnd});
}

void EditorBridge::decreaseSpeed() {
    submitAction({EditorActionType::DecreaseSpeed});
}

void EditorBridge::increaseSpeed() {
    submitAction({EditorActionType::IncreaseSpeed});
}

void EditorBridge::stopReplay() {
    submitAction({EditorActionType::StopReplay});
}

// ===== Edit commands =====

void EditorBridge::splitClip(EditorStateExt& state, const std::string& trackId,
                             const std::string& clipId, int atTick) {
    auto cmd = std::make_unique<video_editing::SplitClipCommand>(trackId, clipId, atTick);
    mCommandStack.push(std::move(cmd), state);
    EventBus::emit(CommandExecutedEvent{"Split Clip", false});
}

void EditorBridge::deleteClip(EditorStateExt& state, const std::string& trackId,
                              const std::string& clipId) {
    auto cmd = std::make_unique<video_editing::RemoveClipCommand>(trackId, clipId);
    mCommandStack.push(std::move(cmd), state);
    EventBus::emit(CommandExecutedEvent{"Delete Clip", false});
}

void EditorBridge::trimClip(EditorStateExt& state, const std::string& trackId,
                            const std::string& clipId, int newInTick, int newOutTick) {
    auto cmd = std::make_unique<video_editing::TrimClipCommand>(trackId, clipId, newInTick, newOutTick);
    mCommandStack.push(std::move(cmd), state);
    EventBus::emit(CommandExecutedEvent{"Trim Clip", false});
}

void EditorBridge::moveClip(EditorStateExt& state, const std::string& trackId,
                            const std::string& clipId, int newTrackTick) {
    auto cmd = std::make_unique<video_editing::MoveClipCommand>(trackId, clipId, newTrackTick);
    mCommandStack.push(std::move(cmd), state);
    EventBus::emit(CommandExecutedEvent{"Move Clip", false});
}

void EditorBridge::addTransition(EditorStateExt& state, const std::string& fromClipId,
                                 const std::string& toClipId, int kind, int durationTicks) {
    auto tk = static_cast<TransitionKind>(kind);
    auto cmd = std::make_unique<video_editing::AddTransitionCommand>(
        fromClipId, toClipId, tk, durationTicks);
    mCommandStack.push(std::move(cmd), state);
    EventBus::emit(CommandExecutedEvent{"Add Transition", false});
}

namespace {
void pushCommand(CommandStack& stack, std::unique_ptr<IEditCommand> command, EditorStateExt& state) {
    if (command) stack.push(std::move(command), state);
}
}

void EditorBridge::splitSequence(EditorStateExt& state, int tick) { pushCommand(mCommandStack, CommandFactory::createSplitSequence(tick), state); }
void EditorBridge::trimSequence(EditorStateExt& state, const std::string& id, int start, int end) { pushCommand(mCommandStack, CommandFactory::createTrimSequence(id, start, end), state); }
void EditorBridge::deleteSequenceSegment(EditorStateExt& state, const std::string& id) { pushCommand(mCommandStack, CommandFactory::createDeleteSequenceSegment(id), state); }
void EditorBridge::bindSequence(EditorStateExt& state, const std::string& id, const std::string& cameraId) { pushCommand(mCommandStack, CommandFactory::createBindSequenceToCamera(id, cameraId), state); }
void EditorBridge::splitWorldActor(EditorStateExt& state, int tick) { pushCommand(mCommandStack, CommandFactory::createSplitWorldActor(tick), state); }
void EditorBridge::trimWorldActor(EditorStateExt& state, const std::string& id, int start, int end) { pushCommand(mCommandStack, CommandFactory::createTrimWorldActor(id, start, end), state); }
void EditorBridge::setWorldActorSegmentSpeed(EditorStateExt& state, const std::string& id, float speed) { pushCommand(mCommandStack, CommandFactory::createSetWorldActorSpeed(id, speed), state); }
void EditorBridge::rippleDeleteWorldActor(EditorStateExt& state, const std::string& id) { pushCommand(mCommandStack, CommandFactory::createRippleDeleteWorldActorSegment(id), state); }
void EditorBridge::addFreeCamera(EditorStateExt& state, const std::string& name) { pushCommand(mCommandStack, CommandFactory::createAddFreeCamera(name), state); }
void EditorBridge::createBindingCamera(EditorStateExt& state, const std::string& id, const std::string& name) { pushCommand(mCommandStack, CommandFactory::createCreateBindingCamera(id, name), state); }
void EditorBridge::deleteCamera(EditorStateExt& state, const std::string& id) { pushCommand(mCommandStack, CommandFactory::createDeleteCamera(id), state); }
void EditorBridge::unbindCamera(EditorStateExt& state, const std::string& id) { pushCommand(mCommandStack, CommandFactory::createUnbindCamera(id), state); }
void EditorBridge::addCameraKeyframe(EditorStateExt& state, const std::string& id, int tick) { pushCommand(mCommandStack, CommandFactory::createAddCameraKeyframe(id, tick), state); }
void EditorBridge::moveCameraKeyframe(EditorStateExt& state, const std::string& id, const std::string& keyframeId, int tick) { pushCommand(mCommandStack, CommandFactory::createMoveCameraKeyframe(id, keyframeId, tick), state); }
void EditorBridge::deleteCameraKeyframe(EditorStateExt& state, const std::string& id, const std::string& keyframeId) { pushCommand(mCommandStack, CommandFactory::createDeleteCameraKeyframe(id, keyframeId), state); }
void EditorBridge::setKeyframeEasing(EditorStateExt& state, const std::string& id, const std::string& keyframeId, EasingType easing) { pushCommand(mCommandStack, CommandFactory::createSetKeyframeEasing(id, keyframeId, easing), state); }
void EditorBridge::setCameraKind(EditorStateExt& state, const std::string& id, CameraKind kind) { pushCommand(mCommandStack, CommandFactory::createSetCameraKind(id, kind), state); }
void EditorBridge::setSubActorDetails(EditorStateExt& state, const std::string& id, AgentDetails details) { pushCommand(mCommandStack, CommandFactory::createSetSubActorDetails(id, std::move(details)), state); }

// ===== Keyframe operations =====

void EditorBridge::addKeyframe(EditorStateExt& state, const std::string& trackId, int tick) {
    for (auto& track : state.cameraTracks) {
        if (track.id != trackId) continue;

        // Check if a keyframe already exists at this tick
        for (const auto& kf : track.keyframes) {
            if (kf.tick == tick) return; // already exists
        }

        CameraKeyframe kf;
        kf.id   = std::to_string(tick) + "_" + trackId;
        kf.tick = tick;
        // Default values from the last keyframe or identity
        if (!track.keyframes.empty()) {
            const auto& last = track.keyframes.back();
            kf.position   = last.position;
            kf.yaw        = last.yaw;
            kf.pitch      = last.pitch;
            kf.tint       = last.tint;
            kf.easingType = last.easingType;
        }
        track.keyframes.push_back(kf);
        std::sort(track.keyframes.begin(), track.keyframes.end(),
            [](const CameraKeyframe& a, const CameraKeyframe& b) { return a.tick < b.tick; });

        EventBus::emit(CommandExecutedEvent{"Add Keyframe", false});
        return;
    }
}

void EditorBridge::moveKeyframe(EditorStateExt& state, const std::string& trackId,
                                const std::string& kfId, int newTick) {
    for (auto& track : state.cameraTracks) {
        if (track.id != trackId) continue;
        for (auto& kf : track.keyframes) {
            if (kf.id != kfId) continue;
            kf.tick = std::clamp(newTick, 0, state.totalTicks);
            std::sort(track.keyframes.begin(), track.keyframes.end(),
                [](const CameraKeyframe& a, const CameraKeyframe& b) { return a.tick < b.tick; });
            return;
        }
    }
}

void EditorBridge::deleteKeyframe(EditorStateExt& state, const std::string& trackId,
                                  const std::string& kfId) {
    for (auto& track : state.cameraTracks) {
        if (track.id != trackId) continue;
        auto it = std::remove_if(track.keyframes.begin(), track.keyframes.end(),
            [&](const CameraKeyframe& kf) { return kf.id == kfId; });
        if (it != track.keyframes.end()) {
            track.keyframes.erase(it, track.keyframes.end());
            EventBus::emit(CommandExecutedEvent{"Delete Keyframe", false});
        }
        return;
    }
}

// ===== Marker operations =====

void EditorBridge::addMarker(EditorStateExt& state, const std::string& label, int tick) {
    Marker m;
    m.id    = "marker_" + std::to_string(tick) + "_" + std::to_string(state.markers.size());
    m.label = label;
    m.tick  = tick;
    state.markers.push_back(m);
    EventBus::emit(CommandExecutedEvent{"Add Marker", false});
}

void EditorBridge::deleteMarker(EditorStateExt& state, const std::string& markerId) {
    auto it = std::remove_if(state.markers.begin(), state.markers.end(),
        [&](const Marker& m) { return m.id == markerId; });
    if (it != state.markers.end()) {
        state.markers.erase(it, state.markers.end());
        EventBus::emit(CommandExecutedEvent{"Delete Marker", false});
    }
}

// ===== Track operations =====

void EditorBridge::addVideoTrack(EditorStateExt& state, const std::string& name) {
    Track t;
    t.id   = "vt_" + std::to_string(state.videoTracks.size() + 1);
    t.name = name.empty() ? "Video Track " + std::to_string(state.videoTracks.size() + 1) : name;
    t.kind = TrackKind::Video;
    t.height = 48;
    state.videoTracks.push_back(t);
    EventBus::emit(CommandExecutedEvent{"Add Video Track", false});
}

void EditorBridge::deleteVideoTrack(EditorStateExt& state, const std::string& trackId) {
    auto it = std::remove_if(state.videoTracks.begin(), state.videoTracks.end(),
        [&](const Track& t) { return t.id == trackId; });
    if (it != state.videoTracks.end()) {
        state.videoTracks.erase(it, state.videoTracks.end());
        EventBus::emit(CommandExecutedEvent{"Delete Video Track", false});
    }
}

// ===== Initialization =====

void EditorBridge::ensureInitialData(EditorStateExt& state) {
    if (state.sequence.empty() && state.totalTicks > 0) {
        state.sequence.push_back({"sequence_1", 0, state.totalTicks});
    }
    if (state.worldActor.segments.empty() && state.totalTicks > 0) {
        state.worldActor.totalTicks = state.totalTicks;
        state.worldActor.segments.push_back({"world_1", 0, state.totalTicks, 0});
    }
    // Create default video track if none exist
    if (state.videoTracks.empty()) {
        Track vt;
        vt.id   = "vt_1";
        vt.name = "Video Track 1";
        vt.kind = TrackKind::Video;
        vt.height = 48;
        state.videoTracks.push_back(vt);
    }

    // Create default camera track if none exist
    if (state.cameraTracks.empty()) {
        CameraTrackExt ct;
        ct.id     = "ct_1";
        ct.name   = "Camera 1";
        ct.active = true;
        ct.visible = true;
        state.cameraTracks.push_back(ct);
    }
}

// ===== Undo/Redo =====

void EditorBridge::undo(EditorStateExt& state) {
    if (mCommandStack.undo(state)) {
        EventBus::emit(CommandExecutedEvent{"Undo", true});
    }
}

void EditorBridge::redo(EditorStateExt& state) {
    if (mCommandStack.redo(state)) {
        EventBus::emit(CommandExecutedEvent{"Redo", false});
    }
}

bool EditorBridge::canUndo() const {
    return mCommandStack.canUndo();
}

bool EditorBridge::canRedo() const {
    return mCommandStack.canRedo();
}

// ===== Private =====

void EditorBridge::submitAction(EditorAction action) {
    mPendingActions.push_back(action);
}

} // namespace playback::refactor::editor
