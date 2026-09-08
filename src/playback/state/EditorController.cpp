#include "EditorController.h"

#include "playback/Playback.h"
#include "playback/io/ReplayLibrary.h"
#include "playback/keyframe/CameraTimelineEvaluator.h"
#include "playback/keyframe/CameraTimelineRegistry.h"
#include "playback/keyframe/ClientCameraCapture.h"
#include "playback/replay/ReplaySession.h"
#include "playback/state/editing/CameraBindingOps.h"
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
    entry.thumbnailPng  = std::move(summary.thumbnailPng);
    return entry;
}

std::string replayPreferenceKey(std::filesystem::path const& path) {
    auto const utf8Path = path.lexically_normal().generic_u8string();
    return {reinterpret_cast<char const*>(utf8Path.data()), utf8Path.size()};
}

constexpr auto kAutosaveInterval = std::chrono::seconds(30);

auto& logger() { return Playback::getInstance().getSelf().getLogger(); }

} // namespace

EditorController::EditorController(EditorContext& context)
: mContext(context),
  mBrowserSnapshot(std::make_shared<ReplayBrowserSnapshot>()),
  mExportDriver(
      std::make_unique<exporting::ReplayExportDriver>(mExportCoordinator, replay::ReplaySession::getInstance())
  ) {}

EditorController::~EditorController() { keyframe::clearCameraTimeline(keyframe::CameraTimelineSource::Preview); }

void EditorController::setRendererAvailable(bool available) {
    if (mExportDriver) mExportDriver->setRendererAvailable(available);
}

void EditorController::publishCameraTimeline() {
    if (mProject.cameras.empty()) {
        keyframe::clearCameraTimeline(keyframe::CameraTimelineSource::Preview);
        return;
    }
    auto dimensionTransitionTicks = replay::ReplaySession::getInstance().getDimensionTransitionTicks();
    keyframe::publishCameraTimeline(
        keyframe::CameraTimelineSource::Preview,
        std::make_shared<keyframe::CameraTimelineEvaluator>(
            mProject,
            mPreviewCameraId,
            std::nullopt,
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
    if (mExportDriver) mExportDriver->reset();
    else mExportCoordinator.reset();
    mBrowserVisible   = false;
    mBrowserOperation = ReplayBrowserOperation::None;
    mBrowserError.clear();
    mBrowserSnapshot = std::make_shared<ReplayBrowserSnapshot>();
    mProject         = {};
    mPreviewCameraId.reset();
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
    mExportTickedBeforeClientUpdate = true;
}

void EditorController::tickExportDuringGraphics() {
    if (!mExportDriver || !mExportDriver->isActive()) return;
    // Advancing runs inside the graphics hook, which the driver itself can re-enter.
    if (mExportTickReentered) return;
    mExportTickReentered = true;
    mExportDriver->tick();
    mExportTickReentered = false;
}

void EditorController::ensureProject(int totalTicks, std::string_view replayPath) {
    totalTicks = std::max(0, totalTicks);
    if (mProjectTotalTicks == totalTicks && mProject.projectPath == replayPath) return;

    // Metadata settles a few frames in; growing the timeline must not discard the loaded project.
    if (mProject.projectPath == replayPath && !replayPath.empty()) {
        mProject.totalTicks            = totalTicks;
        mProject.worldActor.totalTicks = totalTicks;
        for (auto& segment : mProject.worldActor.segments) {
            if (segment.endTick == mProjectTotalTicks) segment.endTick = totalTicks;
        }
        mProjectTotalTicks = totalTicks;
        publishCameraTimeline();
        return;
    }

    mProject             = {};
    mProject.projectPath = std::string(replayPath);
    mProject.totalTicks  = totalTicks;
    state::editing::CameraBindingOps::addFreeCamera(mProject, "Camera 1");
    mProject.worldActor.segments.push_back({"worldActor", 0, totalTicks, 0});
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
    loaded.projectPath           = mProject.projectPath;
    loaded.totalTicks            = mProject.totalTicks;
    loaded.worldActor.totalTicks = mProject.totalTicks;
    if (loaded.cameras.empty()) state::editing::CameraBindingOps::addFreeCamera(loaded, "Camera 1");
    if (loaded.worldActor.segments.empty()) {
        loaded.worldActor.segments.push_back({"worldActor", 0, mProject.totalTicks, 0});
    }
    mProject = std::move(loaded);
    logger().debug("Loaded editor project {}", mProjectFile);
}

bool EditorController::saveProject(std::filesystem::path const& path) {
    if (path.empty()) return false;

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
    case EditorActionType::AddCameraSequence:
        mCommandStack.push(CommandFactory::createAddCameraSequence(), mProject);
        break;
    case EditorActionType::DeleteCameraSequence:
        mCommandStack.push(CommandFactory::createDeleteCameraSequence(), mProject);
        break;
    case EditorActionType::SplitSequence:
        mCommandStack.push(CommandFactory::createSplitSequence(action.tick), mProject);
        break;
    case EditorActionType::TrimSequence:
        mCommandStack.push(CommandFactory::createTrimSequence(action.id, action.tick, action.kind), mProject);
        break;
    case EditorActionType::DeleteSequenceSegment:
        mCommandStack.push(CommandFactory::createDeleteSequenceSegment(action.id), mProject);
        break;
    case EditorActionType::BindSequenceCamera:
        mCommandStack.push(CommandFactory::createBindSequenceToCamera(action.id, action.secondaryId), mProject);
        break;
    case EditorActionType::SplitWorldActor:
        mCommandStack.push(CommandFactory::createSplitWorldActor(action.tick), mProject);
        break;
    case EditorActionType::TrimWorldActor:
        mCommandStack.push(CommandFactory::createTrimWorldActor(action.id, action.tick, action.kind), mProject);
        break;
    case EditorActionType::SetWorldActorSpeed:
        mCommandStack.push(CommandFactory::createSetWorldActorSpeed(action.id, action.speed), mProject);
        break;
    case EditorActionType::RippleDeleteWorldActorSegment:
        mCommandStack.push(CommandFactory::createRippleDeleteWorldActorSegment(action.id), mProject);
        break;
    case EditorActionType::AddCameraKeyframe:
        if (auto captured = captureCameraKeyframe()) {
            logger().debug("Captured camera keyframe (camera={}, tick={})", action.id, action.tick);
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
    case EditorActionType::SetCameraEnabled:
        mCommandStack.push(CommandFactory::createSetCameraEnabled(action.id, action.value), mProject);
        break;
    case EditorActionType::DeleteCamera:
        mCommandStack.push(CommandFactory::createDeleteCamera(action.id), mProject);
        break;
    case EditorActionType::UnbindCamera:
        mCommandStack.push(CommandFactory::createUnbindCamera(action.id), mProject);
        break;
    case EditorActionType::CreateBindingCamera:
        mCommandStack.push(CommandFactory::createCreateBindingCamera(action.id, action.name), mProject);
        break;
    case EditorActionType::SetSubActorDetails:
        mCommandStack.push(CommandFactory::createSetSubActorDetails(action.id, action.details), mProject);
        break;
    case EditorActionType::SetPreviewCamera:
        mPreviewCameraId.reset();
        if (std::ranges::any_of(mProject.cameras, [&](auto const& camera) {
                return camera.id == action.id && state::editing::model::isCameraRenderable(camera);
            })) {
            mPreviewCameraId = action.id;
        }
        break;
    case EditorActionType::ClearPreviewCamera:
        mPreviewCameraId.reset();
        break;
    default:
        break;
    }

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
    // Latched, so opening a menu does not flicker the editor away once it is up.
    if (!sessionActive) mEditorReadyLatched = false;
    else if (hudVisible && session.isReplayWorldReady()) mEditorReadyLatched = true;
    state.editorVisible = mEditorReadyLatched;
    state.hudVisible    = hudVisible;
    state.paused        = session.isPaused();
    state.playbackSpeed = session.getPlaybackSpeed();
    state.currentTick   = std::max(0, session.getCurrentTick());
    state.totalTicks    = std::max(0, session.getTotalTicks());
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
    state.exportStatus      = mExportCoordinator.status();
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
        case EditorActionType::StopReplay:
            if (mExportDriver) mExportDriver->cancel();
            session.requestStop();
            break;
        case EditorActionType::StartExport: {
            auto settings = action.exportSettings.value_or(exporting::ExportSettings{});
            if (!action.exportSettings) {
                settings.startTick = 0;
                settings.endTick   = std::max<int64_t>(0, session.getTotalTicks());
            }
            if (mExportDriver && mExportDriver->start(std::move(settings), mProject, mPreviewCameraId)) {
                publishState(hudVisible);
            }
            break;
        }
        case EditorActionType::CancelExport:
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
        case EditorActionType::LoadProject: {
            auto const target = action.path.empty() ? mProjectFile : action.path;
            if (target.empty()) break;
            state::editing::model::EditorStateExt loaded;
            std::string                           error;
            if (!state::editing::ProjectStore::load(target, loaded, error)) {
                mProjectError = error;
                break;
            }
            loaded.projectPath           = mProject.projectPath;
            loaded.totalTicks            = mProject.totalTicks;
            loaded.worldActor.totalTicks = mProject.totalTicks;
            mProject                     = std::move(loaded);
            mProjectFile                 = target;
            mProjectError.clear();
            mCommandStack.clear();
            mSavedRevision = mCommandStack.revision();
            mPreviewCameraId.reset();
            publishCameraTimeline();
            break;
        }
        }
    }

    if (mExportDriver && !mExportTickedBeforeClientUpdate) mExportDriver->tick();
    mExportTickedBeforeClientUpdate = false;
    autosaveIfDue();
    publishState(hudVisible);
}

} // namespace playback::state
