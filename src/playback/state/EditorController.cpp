#include "EditorController.h"

#include "playback/Playback.h"
#include "playback/audio/PreviewAudio.h"
#include "playback/exporting/ExportPlanCompiler.h"
#include "playback/io/ReplayLibrary.h"
#include "playback/keyframe/CameraTimelineEvaluator.h"
#include "playback/keyframe/CameraTimelineRegistry.h"
#include "playback/keyframe/ClientCameraCapture.h"
#include "playback/replay/ReplaySession.h"
#include "playback/state/editing/CameraBindingOps.h"
#include "playback/state/editing/ClipOps.h"
#include "playback/state/editing/ProjectStore.h"
#include "playback/state/editing/commands/CameraCommands.h"
#include "playback/state/editing/commands/CommandFactory.h"
#include "playback/visuals/FrameTap.h"

#include "ll/api/i18n/I18n.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <utility>

namespace playback::state {

namespace {

ReplayBrowserEntry makeBrowserEntry(io::ReplaySummary summary) {
    ReplayBrowserEntry entry;
    entry.path          = std::move(summary.path);
    entry.replayId      = std::move(summary.replayId);
    entry.replayName    = std::move(summary.replayName);
    entry.worldName     = std::move(summary.worldName);
    entry.durationTicks = summary.durationTicks;
    entry.totalTicks    = summary.totalTicks;
    entry.fileSize      = summary.fileSize;
    entry.lastModified  = summary.lastModified;
    entry.canOpen       = summary.canOpen;
    entry.problem       = std::move(summary.problem);
    return entry;
}

std::string replayPreferenceKey(std::filesystem::path const& path) {
    auto const utf8Path = path.lexically_normal().generic_u8string();
    return {reinterpret_cast<char const*>(utf8Path.data()), utf8Path.size()};
}

constexpr auto kAutosaveInterval = std::chrono::seconds(30);

auto& logger() { return Playback::getInstance().getSelf().getLogger(); }

std::optional<state::editing::model::EditorStateExt>
resolveExportJobProject(state::editing::model::EditorStateExt const& project, QueuedExportJob const& job) {
    std::optional<state::editing::model::EditorStateExt> result;
    if (job.sceneId.empty()) {
        result = project;
    } else {
        auto const* scene = state::editing::ClipOps::findScene(project, job.sceneId);
        if (!scene) return std::nullopt;
        result = state::editing::ClipOps::buildProjectForScene(project, *scene);
    }
    if (job.settings.rangeMode == exporting::ExportRangeMode::SingleClip && job.settings.selectedClipIds.size() == 1) {
        if (auto const hit =
                state::editing::ClipOps::findClipWithCamera(*result, job.settings.selectedClipIds.front())) {
            state::editing::model::CameraEntity isolated = *hit.camera;
            isolated.enabled                             = true;
            for (auto& c : isolated.clips) {
                if (c.id == hit.clip->id) c.exportEnabled = true;
            }
            result->cameras = {std::move(isolated)};
        }
    }
    return result;
}

} // namespace

EditorController::EditorController(EditorContext& context)
: mContext(context),
  mBrowserSnapshot(std::make_shared<ReplayBrowserSnapshot>()),
  mExportDriver(
      std::make_unique<exporting::ReplayExportDriver>(mExportCoordinator, replay::ReplaySession::getInstance())
  ) {}

EditorController::~EditorController() = default;

void EditorController::setRendererAvailable(bool available) {
    if (mExportDriver) mExportDriver->setRendererAvailable(available);
}

void EditorController::publishCameraTimeline() {
    if (mProject.cameras.empty()) {
        keyframe::clearCameraTimeline(keyframe::CameraTimelineSource::Preview);
        return;
    }
    auto dimensionTransitionTicks = replay::ReplaySession::getInstance().getDimensionTransitionTicks();
    // No subject sampler: the replay session follows a person track's subject natively, like the user's follow.
    keyframe::publishCameraTimeline(
        keyframe::CameraTimelineSource::Preview,
        std::make_shared<keyframe::CameraTimelineEvaluator>(
            mProject,
            mPreviewCameraId,
            false,
            std::move(dimensionTransitionTicks)
        )
    );
}

std::optional<state::editing::model::CameraKeyframe> EditorController::captureCameraKeyframe() const {
    auto const captured = keyframe::captureClientCamera();
    if (!captured) return std::nullopt;
    auto const&                           state = *captured;
    state::editing::model::CameraKeyframe key;
    key.position = {state.x, state.y, state.z};
    key.yaw      = state.yaw;
    key.pitch    = state.pitch;
    key.roll     = state.roll;
    key.fov      = state.fov;
    return key;
}

void EditorController::reset() {
    flushProjectOnClose();
    clearExportBatch();
    if (mExportDriver) mExportDriver->reset();
    else mExportCoordinator.reset();
    mBrowserVisible   = false;
    mBrowserOperation = ReplayBrowserOperation::None;
    mBrowserError.clear();
    mBrowserSnapshot = std::make_shared<ReplayBrowserSnapshot>();
    mProject         = {};
    mPreviewCameraId.reset();
    audio::preview::setClipMuted(false);
    keyframe::clearCameraTimeline(keyframe::CameraTimelineSource::Preview);
    mCommandStack.clear();
    mActiveReplayPath.clear();
    mProjectFile.clear();
    mProjectError.clear();
    mSavedRevision                  = mCommandStack.revision();
    mEditorReadyLatched             = false;
    mProjectTotalTicks              = -1;
    mExportTickedBeforeClientUpdate = false;
}

void EditorController::tickExportBeforeClientUpdate() {
    mExportTickedBeforeClientUpdate = false;
    if (!mExportDriver || !mExportDriver->isActive()) return;
    mExportDriver->tick();
    advanceExportBatchIfNeeded();
    mExportTickedBeforeClientUpdate = true;
}

void EditorController::tickExportDuringGraphics() {
    if (!mExportDriver || !mExportDriver->isActive()) return;
    // Advancing runs inside the graphics hook, which the driver itself can re-enter.
    if (mExportTickReentered) return;
    mExportTickReentered = true;
    mExportDriver->tick();
    advanceExportBatchIfNeeded();
    mExportTickReentered = false;
}

void EditorController::ensureProject(int totalTicks, std::string_view replayPath) {
    totalTicks = std::max(0, totalTicks);
    if (mProjectTotalTicks == totalTicks && mProject.projectPath == replayPath) return;

    // Metadata settles a few frames in; growing the timeline must not discard the loaded project.
    if (mProject.projectPath == replayPath && !replayPath.empty()) {
        int const previousTotalTicks = mProject.totalTicks;
        mProject.totalTicks          = totalTicks;
        mProjectTotalTicks           = totalTicks;
        state::editing::ClipOps::ensureDefaultScene(mProject);
        state::editing::ClipOps::syncTotalTicks(mProject, previousTotalTicks);
        publishCameraTimeline();
        return;
    }

    mProject             = {};
    mProject.projectPath = std::string(replayPath);
    mProject.totalTicks  = totalTicks;
    state::editing::CameraBindingOps::addFreeCamera(mProject, {});
    state::editing::ClipOps::ensureDefaultScene(mProject);
    mCommandStack.clear();
    mPreviewCameraId.reset();
    mProjectTotalTicks = totalTicks;
    loadProjectForReplay(replayPath);
    mSavedRevision = mCommandStack.revision();
    mLastAutosave  = std::chrono::steady_clock::now();
    publishCameraTimeline();
}

void EditorController::loadProjectForReplay(std::string_view replayPath) {
    mProjectFile.clear();
    mProjectError.clear();
    if (replayPath.empty()) return;

    mProjectFile = state::editing::ProjectStore::defaultProjectPath(replayPath);
    if (!state::editing::ProjectStore::exists(mProjectFile)) return;

    state::editing::model::EditorStateExt loaded;
    std::string                           error;
    if (!state::editing::ProjectStore::load(mProjectFile, loaded, error)) {
        mProjectError = error;
        logger().warn("Unable to load editor project {}: {}", mProjectFile, error);
        return;
    }

    // The replay on disk is authoritative for length; a stale project must not shrink the timeline.
    int const loadedTotalTicks = loaded.totalTicks;
    loaded.projectPath         = mProject.projectPath;
    loaded.totalTicks          = mProject.totalTicks;
    state::editing::ClipOps::ensureDefaultScene(loaded);
    state::editing::ClipOps::syncTotalTicks(loaded, loadedTotalTicks);
    mProject = std::move(loaded);
    logger().debug("Loaded editor project {}", mProjectFile);
}

bool EditorController::saveProject(std::filesystem::path const& path) {
    if (path.empty()) return false;

    state::editing::ClipOps::syncActiveSceneFromWorking(mProject);
    std::string error;
    if (!state::editing::ProjectStore::save(mProject, path, error)) {
        mProjectError = error;
        logger().error("Unable to save editor project {}: {}", path, error);
        return false;
    }
    mProjectFile = path;
    mProjectError.clear();
    mSavedRevision = mCommandStack.revision();
    mLastAutosave  = std::chrono::steady_clock::now();
    logger().debug("Saved editor project {}", path);
    return true;
}

void EditorController::autosaveIfDue() {
    if (mProjectFile.empty() || !isProjectDirty()) return;
    if (mExportDriver && mExportDriver->isActive()) return;

    auto const now = std::chrono::steady_clock::now();
    if (now - mLastAutosave < kAutosaveInterval) return;
    mLastAutosave = now;
    (void)saveProject(mProjectFile);
}

void EditorController::flushProjectOnClose() {
    if (mProjectFile.empty() || !isProjectDirty()) return;
    (void)saveProject(mProjectFile);
}

void EditorController::applyEditorAction(EditorAction const& action) {
    using namespace state::editing::command;

    state::editing::ClipOps::syncActiveSceneFromWorking(mProject);

    switch (action.type) {
    case EditorActionType::UndoEditorEdit:
        (void)mCommandStack.undo(mProject);
        break;
    case EditorActionType::RedoEditorEdit:
        (void)mCommandStack.redo(mProject);
        break;
    case EditorActionType::AddFreeCamera:
        mCommandStack.push(CommandFactory::createAddFreeCamera(action.name), mProject);
        break;
    case EditorActionType::AddCameraKeyframe:
        if (action.keyframe) {
            mCommandStack.push(
                CommandFactory::createAddCameraKeyframe(action.id, action.tick, action.keyframe),
                mProject
            );
            break;
        }
        if (auto captured = captureCameraKeyframe()) {
            logger().debug("Captured camera keyframe (camera={}, tick={})", action.id, action.tick);
            // A person track's first key views whoever the user follows; later keys keep the previous subject.
            captured->subject = replay::ReplaySession::getInstance().userReplaySubjectChoice();
            mCommandStack.push(
                CommandFactory::createAddCameraKeyframe(action.id, action.tick, std::move(captured)),
                mProject
            );
        } else {
            logger().debug(
                "Camera keyframe capture unavailable (camera={}, tick={}); using defaults",
                action.id,
                action.tick
            );
            mCommandStack.push(CommandFactory::createAddCameraKeyframe(action.id, action.tick), mProject);
        }
        break;
    case EditorActionType::MoveCameraKeyframe:
        mCommandStack.push(
            CommandFactory::createMoveCameraKeyframe(action.id, action.tick, action.secondaryTick),
            mProject
        );
        break;
    case EditorActionType::DeleteCameraKeyframe:
        mCommandStack.push(CommandFactory::createDeleteCameraKeyframe(action.id, action.tick), mProject);
        break;
    case EditorActionType::SetKeyframeMode:
        mCommandStack.push(
            CommandFactory::createSetCameraKeyframeMode(
                action.id,
                action.tick,
                static_cast<state::editing::model::CameraKeyframeMode>(action.kind)
            ),
            mProject
        );
        break;
    case EditorActionType::SetKeyframeInterpolation:
        mCommandStack.push(
            CommandFactory::createSetKeyframeInterpolation(
                action.id,
                action.tick,
                static_cast<state::editing::model::CameraInterpolationType>(action.kind)
            ),
            mProject
        );
        break;
    case EditorActionType::SetKeyframeBezierControls:
        mCommandStack.push(
            CommandFactory::createSetCameraKeyframeBezierControls(
                action.id,
                action.tick,
                action.bezierCtrl1,
                action.bezierCtrl2
            ),
            mProject
        );
        break;
    case EditorActionType::SetKeyframePosition:
        mCommandStack.push(
            CommandFactory::createSetCameraKeyframePosition(action.id, action.tick, action.position),
            mProject
        );
        break;
    case EditorActionType::SetKeyframeRotation:
        mCommandStack.push(
            CommandFactory::createSetCameraKeyframeRotation(action.id, action.tick, action.position),
            mProject
        );
        break;
    case EditorActionType::SetKeyframeFov:
        mCommandStack.push(CommandFactory::createSetCameraKeyframeFov(action.id, action.tick, action.speed), mProject);
        break;
    case EditorActionType::SetKeyframeSubject:
        mCommandStack.push(
            CommandFactory::createSetCameraKeyframeSubject(action.id, action.tick, action.secondaryId),
            mProject
        );
        break;
    case EditorActionType::SetCameraEnabled:
        mCommandStack.push(CommandFactory::createSetCameraEnabled(action.id, action.value), mProject);
        break;
    case EditorActionType::SetCameraLocked:
        mCommandStack.push(CommandFactory::createSetCameraLocked(action.id, action.value), mProject);
        break;
    case EditorActionType::SetCameraShake:
        mCommandStack.push(CommandFactory::createSetCameraShake(action.id, action.shake), mProject);
        break;
    case EditorActionType::RenameCamera:
        mCommandStack.push(CommandFactory::createRenameCamera(action.id, action.name), mProject);
        break;
    case EditorActionType::MoveCamera:
        mCommandStack.push(CommandFactory::createMoveCamera(action.id, action.kind), mProject);
        break;
    case EditorActionType::ClearCameraKeyframes:
        mCommandStack.push(CommandFactory::createClearCameraKeyframes(action.id), mProject);
        break;
    case EditorActionType::DeleteCamera:
        mCommandStack.push(CommandFactory::createDeleteCamera(action.id), mProject);
        break;
    case EditorActionType::MoveCameraKeyframes:
        mCommandStack.push(CommandFactory::createMoveCameraKeyframes(action.keyframeRefs, action.tick), mProject);
        break;
    case EditorActionType::DeleteCameraKeyframes:
        mCommandStack.push(CommandFactory::createDeleteCameraKeyframes(action.keyframeRefs), mProject);
        break;
    case EditorActionType::SetKeyframesMode:
        mCommandStack.push(
            CommandFactory::createSetKeyframesMode(
                action.keyframeRefs,
                static_cast<state::editing::model::CameraKeyframeMode>(action.kind)
            ),
            mProject
        );
        break;
    case EditorActionType::SetKeyframesInterpolation:
        mCommandStack.push(
            CommandFactory::createSetKeyframesInterpolation(
                action.keyframeRefs,
                static_cast<state::editing::model::CameraInterpolationType>(action.kind)
            ),
            mProject
        );
        break;
    case EditorActionType::SetKeyframesSubject:
        mCommandStack.push(
            CommandFactory::createSetKeyframesSubject(action.keyframeRefs, action.secondaryId),
            mProject
        );
        break;
    case EditorActionType::AddCameraKeyframes:
        mCommandStack.push(CommandFactory::createAddCameraKeyframes(action.keyframeRefs), mProject);
        break;
    case EditorActionType::AddMarker:
        mCommandStack.push(CommandFactory::createAddMarker(action.id, action.tick, action.name), mProject);
        break;
    case EditorActionType::DeleteMarker:
        mCommandStack.push(CommandFactory::createDeleteMarker(action.id), mProject);
        break;
    case EditorActionType::MoveMarker:
        mCommandStack.push(CommandFactory::createMoveMarker(action.id, action.tick), mProject);
        break;
    case EditorActionType::RenameMarker:
        mCommandStack.push(CommandFactory::createRenameMarker(action.id, action.name), mProject);
        break;
    case EditorActionType::SetKeyframeIgnoreClipSpeed:
        mCommandStack.push(
            CommandFactory::createSetCameraKeyframeIgnoreClipSpeed(action.id, action.tick, action.value),
            mProject
        );
        break;
    case EditorActionType::SetKeyframesIgnoreClipSpeed:
        mCommandStack.push(
            CommandFactory::createSetKeyframesIgnoreClipSpeed(action.keyframeRefs, action.value),
            mProject
        );
        break;
    case EditorActionType::AddClip:
        mCommandStack.push(
            CommandFactory::createAddClip(
                action.id,
                action.tick,
                action.secondaryTick,
                action.name,
                action.speed > 0.0f ? action.speed : 1.0f
            ),
            mProject
        );
        break;
    case EditorActionType::SplitClip:
        mCommandStack.push(CommandFactory::createSplitClip(action.id, action.tick, action.name), mProject);
        break;
    case EditorActionType::MoveAndTrimClip:
        mCommandStack.push(
            CommandFactory::createMoveAndTrimClip(action.id, action.tick, action.secondaryTick),
            mProject
        );
        break;
    case EditorActionType::SetClipSpeed:
        mCommandStack.push(CommandFactory::createSetClipSpeed(action.id, action.speed), mProject);
        break;
    case EditorActionType::RenameClip:
        mCommandStack.push(CommandFactory::createRenameClip(action.id, action.name), mProject);
        break;
    case EditorActionType::SetClipMuted:
        mCommandStack.push(CommandFactory::createSetClipMuted(action.id, action.value), mProject);
        break;
    case EditorActionType::SetClipLocked:
        mCommandStack.push(CommandFactory::createSetClipLocked(action.id, action.value), mProject);
        break;
    case EditorActionType::SetClipExportEnabled:
        mCommandStack.push(CommandFactory::createSetClipExportEnabled(action.id, action.value), mProject);
        break;
    case EditorActionType::SetAllClipsExportEnabled:
        mCommandStack.push(CommandFactory::createSetAllClipsExportEnabled(action.value, action.id), mProject);
        break;
    case EditorActionType::ResetClipsToFullRange:
        mCommandStack.push(CommandFactory::createResetClipsToFullRange(action.id, action.name), mProject);
        break;
    case EditorActionType::DeleteClip:
        mCommandStack.push(CommandFactory::createDeleteClip(action.id), mProject);
        break;
    case EditorActionType::AddScene:
        mCommandStack.push(CommandFactory::createAddScene(action.name), mProject);
        break;
    case EditorActionType::DuplicateScene:
        mCommandStack.push(CommandFactory::createDuplicateScene(action.id, action.name), mProject);
        break;
    case EditorActionType::RenameScene:
        mCommandStack.push(CommandFactory::createRenameScene(action.id, action.name), mProject);
        break;
    case EditorActionType::MoveScene:
        mCommandStack.push(CommandFactory::createMoveScene(action.id, action.kind), mProject);
        break;
    case EditorActionType::DeleteScene:
        mCommandStack.push(CommandFactory::createDeleteScene(action.id), mProject);
        break;
    case EditorActionType::SwitchScene:
        if (!action.id.empty() && action.id != mProject.activeSceneId
            && state::editing::ClipOps::findScene(mProject, action.id) != nullptr) {
            state::editing::ClipOps::syncActiveSceneFromWorking(mProject);
            mProject.activeSceneId = action.id;
            state::editing::ClipOps::loadWorkingFromActiveScene(mProject);
        }
        break;
    case EditorActionType::SetSceneExportRange:
        mCommandStack.push(CommandFactory::createSetSceneExportRange(action.tick, action.secondaryTick), mProject);
        break;
    case EditorActionType::SetPreviewCamera:
        replay::ReplaySession::getInstance().setFollowRecordedPlayer(false);
        mPreviewCameraId.reset();
        if (std::ranges::any_of(mProject.cameras, [&](auto const& camera) {
                return camera.id == action.id && state::editing::model::isCameraRenderable(camera);
            })) {
            mPreviewCameraId = action.id;
        }
        break;
    case EditorActionType::ClearPreviewCamera:
        replay::ReplaySession::getInstance().setFollowRecordedPlayer(false);
        mPreviewCameraId.reset();
        break;
    case EditorActionType::MoveViewToKeyframe: {
        auto const camera = std::ranges::find(mProject.cameras, action.id, &state::editing::model::CameraEntity::id);
        if (camera == mProject.cameras.end()) break;
        auto const key = camera->keysByTick.find(action.tick);
        if (key == camera->keysByTick.end()) break;
        auto& session = replay::ReplaySession::getInstance();
        session.setFollowRecordedPlayer(false);
        mPreviewCameraId.reset();
        auto const& pose = key->second;
        session.requestParkObserver(
            keyframe::CameraRenderState{
                pose.position.x,
                pose.position.y,
                pose.position.z,
                pose.yaw,
                pose.pitch,
                pose.roll,
                pose.fov,
            }
        );
        break;
    }
    default:
        break;
    }

    state::editing::ClipOps::syncActiveSceneFromWorking(mProject);

    if (mPreviewCameraId && !std::ranges::any_of(mProject.cameras, [&](auto const& camera) {
            return camera.id == *mPreviewCameraId && state::editing::model::isCameraRenderable(camera);
        })) {
        mPreviewCameraId.reset();
    }
    publishCameraTimeline();
}

void EditorController::publishState(bool hudVisible) {
    auto& session = replay::ReplaySession::getInstance();

    bool const sessionActive = session.isActive();

    EditorState state;
    state.replayVisible = sessionActive && session.hasJoinedReplayWorld();
    // Gate only on the loading/progress screen clearing and let chunks stream in natively.
    if (!sessionActive) mEditorReadyLatched = false;
    else if (hudVisible) mEditorReadyLatched = true;
    state.editorVisible           = mEditorReadyLatched;
    state.hudVisible              = hudVisible;
    state.paused                  = session.isPaused();
    state.followingRecordedPlayer = session.isFollowingRecordedPlayer();
    state.followThirdPerson       = session.isFollowThirdPerson();
    state.replaySubjectChoice     = session.userReplaySubjectChoice();
    auto const replayPlayers      = session.replayPlayers();
    state.replayPlayers.reserve(replayPlayers.size());
    for (auto const& player : replayPlayers) {
        state.replayPlayers.push_back({player.uuid, player.name, player.recorder, player.present});
    }
    auto const replayEntities = session.replayEntities();
    state.replayEntities.reserve(replayEntities.size());
    for (auto const& entity : replayEntities) {
        if (!entity.player) state.replayEntities.push_back({entity.choice, entity.label, entity.distance});
    }
    state.playbackSpeed = session.getPlaybackSpeed();
    state.currentTick             = std::max(0, session.getCurrentTick());
    state.totalTicks              = std::max(0, session.getTotalTicks());
    if (state.editorVisible != mEditorVisibleLogged) {
        mEditorVisibleLogged = state.editorVisible;
        logger().debug(
            "Replay editor {} (joined={}, worldReady={}, hud={}, totalTicks={})",
            state.editorVisible ? "shown" : "hidden",
            session.hasJoinedReplayWorld(),
            session.isReplayWorldReady(),
            hudVisible,
            state.totalTicks
        );
    }
    if (!sessionActive) {
        flushProjectOnClose();
        mActiveReplayPath.clear();
    }
    ensureProject(state.totalTicks, mActiveReplayPath);
    mProject.currentTick             = state.currentTick;
    mProject.playing                 = !state.paused;
    mProject.playbackSpeed           = state.playbackSpeed;
    state.project                    = std::make_shared<state::editing::model::EditorStateExt>(mProject);
    state.cameraTimeline             = keyframe::currentCameraTimeline(keyframe::CameraTimelineSource::Preview);
    if (state.cameraTimeline) {
        if (auto const shot = state.cameraTimeline->shotAt(state.currentTick)) state.activeCameraId = shot.camera->id;
    }
    state.canUndo                    = mCommandStack.canUndo();
    state.canRedo                    = mCommandStack.canRedo();
    state.persistence.dirty          = state.editorVisible && isProjectDirty();
    state.persistence.projectFile    = mProjectFile.empty() ? std::string{} : mProjectFile.filename().string();
    state.persistence.error          = mProjectError;
    state.capabilities.cameraEditing = state.editorVisible;
    state.capabilities.videoEditing  = state.editorVisible;
    state.capabilities.videoExport   = state.editorVisible && mExportDriver && mExportDriver->isAvailable();
    state.capabilities.ffmpegVideoExport =
        state.capabilities.videoExport
        && exporting::ExportCoordinator::isFormatAvailable(exporting::ExportFormat::Mp4Video);
    state.exportStatus = mExportCoordinator.status();
    if (!mPendingExportBatch.empty() && state.exportStatus.state == exporting::ExportState::Completed) {
        state.exportStatus.state = exporting::ExportState::Preparing;
    }
    if (mExportBatchTotal > 1) {
        state.exportStatus.batchIndex = mExportBatchIndex;
        state.exportStatus.batchTotal = mExportBatchTotal;
        state.exportStatus.batchLabel = mExportBatchLabel;
    }
    state.browser.visible   = mBrowserVisible;
    state.browser.operation = mBrowserOperation;
    state.browser.error     = mBrowserError;
    state.browser.snapshot  = mBrowserSnapshot;
    mContext.publish(std::move(state));
}

void EditorController::refreshBrowser() {
    auto snapshot      = std::make_shared<ReplayBrowserSnapshot>();
    snapshot->revision = ++mBrowserRevision;
    auto replays       = io::ReplayLibrary::loadReplays();
    snapshot->replays.reserve(replays.size());
    for (auto& replay : replays) snapshot->replays.emplace_back(makeBrowserEntry(std::move(replay)));
    mBrowserSnapshot = std::move(snapshot);
}

ReplayBrowserEntry const* EditorController::findBrowserEntry(std::string_view replayId) const {
    if (!mBrowserSnapshot) return nullptr;
    auto const it = std::find_if(
        mBrowserSnapshot->replays.begin(),
        mBrowserSnapshot->replays.end(),
        [replayId](ReplayBrowserEntry const& entry) { return entry.replayId == replayId; }
    );
    return it == mBrowserSnapshot->replays.end() ? nullptr : &*it;
}

void EditorController::clearExportBatch() {
    mPendingExportBatch.clear();
    mExportBatchIndex = 0;
    mExportBatchTotal = 0;
    mExportBatchLabel.clear();
}

void EditorController::startExportBatch(std::vector<QueuedExportJob> jobs, bool hudVisible) {
    clearExportBatch();
    if (!mExportDriver || jobs.empty()) return;

    state::editing::ClipOps::syncActiveSceneFromWorking(mProject);

    for (auto const& job : jobs) {
        auto jobProject = resolveExportJobProject(mProject, job);
        if (!jobProject) {
            mExportCoordinator.fail(exporting::ExportError::InvalidTimeline, "The selected scene no longer exists");
            publishState(hudVisible);
            return;
        }
        auto compiled = exporting::ExportPlanCompiler::compile(job.settings, *jobProject);
        if (!compiled) {
            mExportCoordinator.fail(compiled.error, std::move(compiled.message));
            publishState(hudVisible);
            return;
        }
    }

    mExportBatchTotal = static_cast<uint32_t>(jobs.size());
    mExportBatchIndex = 1;
    mExportBatchLabel = jobs.front().label;
    for (size_t i = 1; i < jobs.size(); ++i) mPendingExportBatch.push_back(std::move(jobs[i]));

    auto       firstProject   = resolveExportJobProject(mProject, jobs.front());
    auto const cameraFallback = (jobs.front().sceneId.empty() || jobs.front().sceneId == mProject.activeSceneId)
                                  ? mPreviewCameraId
                                  : std::nullopt;
    if (!firstProject || !mExportDriver->start(std::move(jobs.front().settings), *firstProject, cameraFallback)) {
        clearExportBatch();
        return;
    }
    publishState(hudVisible);
}

void EditorController::advanceExportBatchIfNeeded() {
    if (mPendingExportBatch.empty() || !mExportDriver || mExportDriver->isActive()) return;
    auto const status = mExportCoordinator.status();
    if (exporting::isExportActive(status.state)) return;
    if (status.state != exporting::ExportState::Completed) {
        clearExportBatch();
        return;
    }

    auto nextJob = std::move(mPendingExportBatch.front());
    mPendingExportBatch.pop_front();
    ++mExportBatchIndex;
    mExportBatchLabel = nextJob.label;

    auto jobProject = resolveExportJobProject(mProject, nextJob);
    if (!jobProject) {
        clearExportBatch();
        mExportCoordinator.fail(exporting::ExportError::InvalidTimeline, "The selected scene no longer exists");
        return;
    }
    auto const cameraFallback =
        (nextJob.sceneId.empty() || nextJob.sceneId == mProject.activeSceneId) ? mPreviewCameraId : std::nullopt;
    mExportDriver->reset();
    if (!mExportDriver->start(std::move(nextJob.settings), *jobProject, cameraFallback)) {
        clearExportBatch();
    }
}

void EditorController::tick(bool hudVisible) {
    using namespace ll::i18n_literals;

    auto& session = replay::ReplaySession::getInstance();

    for (auto const& action : mContext.takeActions()) {
        if (mExportDriver && mExportDriver->isActive() && action.type != EditorActionType::CancelExport
            && action.type != EditorActionType::StopReplay) {
            continue;
        }
        if (action.type >= EditorActionType::UndoEditorEdit) {
            applyEditorAction(action);
            continue;
        }
        switch (action.type) {
        case EditorActionType::TogglePause:
            (void)session.setPaused(!session.isPaused());
            break;
        case EditorActionType::Seek:
            session.requestSeek(action.tick);
            break;
        case EditorActionType::DecreaseSpeed:
            session.adjustPlaybackSpeed(-1);
            break;
        case EditorActionType::IncreaseSpeed:
            session.adjustPlaybackSpeed(1);
            break;
        case EditorActionType::SetPlaybackSpeed:
            session.setPlaybackSpeed(action.speed);
            break;
        case EditorActionType::StopReplay:
            clearExportBatch();
            if (mExportDriver) mExportDriver->cancel();
            session.requestStop();
            break;
        case EditorActionType::StartExport: {
            std::vector<QueuedExportJob> jobs = action.exportJobs;
            if (jobs.empty()) {
                auto settings = action.exportSettings.value_or(exporting::ExportSettings{});
                if (!action.exportSettings) {
                    settings.rangeMode = exporting::ExportRangeMode::SequenceInOut;
                    settings.startTick = 0;
                    settings.endTick   = std::max<int64_t>(0, session.getTotalTicks());
                }
                jobs.push_back(QueuedExportJob{std::move(settings), {}, {}});
            }
            startExportBatch(std::move(jobs), hudVisible);
            break;
        }
        case EditorActionType::CancelExport:
            clearExportBatch();
            if (mExportDriver) mExportDriver->cancel();
            break;
        case EditorActionType::OpenReplayBrowser:
            if (!session.isActive()) {
                mBrowserVisible = true;
                mBrowserError.clear();
                runBrowserOperation(ReplayBrowserOperation::Refreshing, hudVisible, [this] { refreshBrowser(); });
            }
            break;
        case EditorActionType::CloseReplayBrowser:
            mBrowserVisible = false;
            mBrowserError.clear();
            break;
        case EditorActionType::RefreshReplayBrowser:
            runBrowserOperation(ReplayBrowserOperation::Refreshing, hudVisible, [this] {
                mBrowserError.clear();
                refreshBrowser();
            });
            break;
        case EditorActionType::OpenReplay:
            runBrowserOperation(ReplayBrowserOperation::OpeningReplay, hudVisible, [&] {
                auto replay = action.path.empty() ? io::ReplayLibrary::findReplay(action.replayId)
                                                  : io::ReplayLibrary::findReplay(action.path.string());
                if (!replay) {
                    mBrowserError = "playback.replayBrowser.error.fileNotFound"_tr();
                } else if (!replay->canOpen) {
                    mBrowserError =
                        replay->problem.empty() ? "playback.replayBrowser.error.invalidArchive"_tr() : replay->problem;
                } else if (!session.start(replay->path)) {
                    mBrowserError = "playback.replayBrowser.error.openFailed"_tr();
                } else {
                    mActiveReplayPath = replayPreferenceKey(replay->path);
                    mBrowserVisible   = false;
                    mBrowserError.clear();
                }
            });
            break;
        case EditorActionType::ImportReplay:
            runBrowserOperation(ReplayBrowserOperation::ImportingReplay, hudVisible, [&] {
                if (io::ReplayLibrary::importReplay(action.path, mBrowserError)) refreshBrowser();
            });
            break;
        case EditorActionType::DeleteReplays:
            runBrowserOperation(ReplayBrowserOperation::DeletingReplay, hudVisible, [&] {
                mBrowserError.clear();
                bool changed = false;
                for (auto const& replayId : action.replayIds) {
                    auto const* entry = findBrowserEntry(replayId);
                    if (!entry) {
                        mBrowserError = "playback.replayBrowser.error.fileNotFound"_tr();
                        break;
                    }
                    auto replay = io::ReplayLibrary::findReplay(entry->path.string());
                    if (!replay || !io::ReplayLibrary::deleteReplay(*replay, mBrowserError)) break;
                    changed = true;
                }
                if (changed) refreshBrowser();
            });
            break;
        case EditorActionType::RenameReplay:
            runBrowserOperation(ReplayBrowserOperation::RenamingReplay, hudVisible, [&] {
                auto const* entry  = findBrowserEntry(action.replayId);
                auto        replay = entry ? io::ReplayLibrary::findReplay(entry->path.string()) : std::nullopt;
                if (!replay) {
                    mBrowserError = "playback.replayBrowser.error.fileNotFound"_tr();
                } else if (io::ReplayLibrary::renameReplay(*replay, action.name, mBrowserError)) {
                    refreshBrowser();
                }
            });
            break;
        case EditorActionType::ShowReplayInFolder:
            runBrowserOperation(ReplayBrowserOperation::ShowingInFolder, hudVisible, [&] {
                auto const* entry  = findBrowserEntry(action.replayId);
                auto        replay = entry ? io::ReplayLibrary::findReplay(entry->path.string()) : std::nullopt;
                if (!replay) {
                    mBrowserError = "playback.replayBrowser.error.fileNotFound"_tr();
                } else if (!io::ReplayLibrary::showInFolder(*replay)) {
                    mBrowserError = "playback.replayBrowser.error.showInFolderFailed"_tr();
                } else {
                    mBrowserError.clear();
                }
            });
            break;
        case EditorActionType::ClearReplayBrowserError:
            mBrowserError.clear();
            break;
        case EditorActionType::SaveProject: {
            auto target = action.path.empty() ? mProjectFile : action.path;
            if (target.empty()) target = state::editing::ProjectStore::defaultProjectPath(mProject.projectPath);
            if (!target.empty()) (void)saveProject(target);
            break;
        }
        case EditorActionType::TeleportToRecordedPlayer:
            session.requestTeleportToRecordedPlayer();
            break;
        case EditorActionType::SetFollowRecordedPlayer:
            if (action.value) session.setReplaySubjectChoice(action.id);
            session.setFollowRecordedPlayer(action.value);
            break;
        case EditorActionType::SetFollowThirdPerson:
            session.setFollowThirdPerson(action.value);
            break;
        case EditorActionType::SetReplaySubjectChoice: {
            bool legal = action.id.empty();
            if (!legal) {
                for (auto const& player : session.replayPlayers()) {
                    if (player.uuid == action.id && player.present) {
                        legal = true;
                        break;
                    }
                }
            }
            if (!legal) {
                logger().debug("replay subject ignored uuid={}", action.id);
                break;
            }
            session.setReplaySubjectChoice(action.id);
            break;
        }
        case EditorActionType::LoadProject: {
            auto const target = action.path.empty() ? mProjectFile : action.path;
            if (target.empty()) break;
            state::editing::model::EditorStateExt loaded;
            std::string                           error;
            if (!state::editing::ProjectStore::load(target, loaded, error)) {
                mProjectError = error;
                break;
            }
            int const loadedTotalTicks = loaded.totalTicks;
            loaded.projectPath         = mProject.projectPath;
            loaded.totalTicks          = mProject.totalTicks;
            state::editing::ClipOps::ensureDefaultScene(loaded);
            state::editing::ClipOps::syncTotalTicks(loaded, loadedTotalTicks);
            mProject                     = std::move(loaded);
            mProjectFile                 = target;
            mProjectError.clear();
            mCommandStack.clear();
            mSavedRevision = mCommandStack.revision();
            mPreviewCameraId.reset();
            publishCameraTimeline();
            break;
        }
        default:
            break;
        }
    }

    if (mExportDriver && !mExportTickedBeforeClientUpdate) mExportDriver->tick();
    mExportTickedBeforeClientUpdate = false;
    advanceExportBatchIfNeeded();
    autosaveIfDue();
    publishState(hudVisible);
    if (session.isActive() && (!mExportDriver || !mExportDriver->isActive())) {
        int const currentTick = std::max(0, session.getCurrentTick());
        session.setClipSpeedScale(state::editing::ClipOps::activeClipSpeedAt(mProject, currentTick));
        audio::preview::setClipMuted(state::editing::ClipOps::isClipMutedAt(mProject, currentTick));
    } else {
        session.setClipSpeedScale(1.0f);
        audio::preview::setClipMuted(false);
    }
}

} // namespace playback::state
