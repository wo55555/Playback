#include "EditorController.h"

#include "playback/editor/editing/CameraBindingOps.h"
#include "playback/editor/editing/commands/CameraCommands.h"
#include "playback/editor/editing/commands/CommandFactory.h"
#include "playback/editor/keyframe/CameraTimelineEvaluator.h"
#include "playback/editor/keyframe/CameraTimelineRegistry.h"
#include "playback/functions/render/FrameTap.h"
#include "playback/functions/replay/ReplaySession.h"
#include "playback/screen/ReplayBrowser.h"

#include "ll/api/i18n/I18n.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/deps/renderer/Camera.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

namespace playback::editor {

namespace {

ReplayBrowserEntry makeBrowserEntry(screen::ReplaySummary summary) {
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

} // namespace

EditorController::EditorController(EditorContext& context)
: mContext(context),
  mBrowserSnapshot(std::make_shared<ReplayBrowserSnapshot>()),
  mExportDriver(
      std::make_unique<exporting::ReplayExportDriver>(mExportCoordinator, functions::ReplaySession::getInstance())
  ) {}

EditorController::~EditorController() { keyframe::clearCameraTimeline(keyframe::CameraTimelineSource::Preview); }

void EditorController::setFrameTap(functions::render::FrameTap* frameTap) {
    if (mExportDriver) mExportDriver->setFrameTap(frameTap);
}

void EditorController::publishCameraTimeline() {
    if (mProject.cameras.empty()) {
        keyframe::clearCameraTimeline(keyframe::CameraTimelineSource::Preview);
        return;
    }
    keyframe::publishCameraTimeline(
        keyframe::CameraTimelineSource::Preview,
        std::make_shared<keyframe::CameraTimelineEvaluator>(mProject, mPreviewCameraId)
    );
}

std::optional<editing::model::CameraKeyframe> EditorController::captureCameraKeyframe() const {
    auto client = ll::service::getClientInstance();
    if (!client) return std::nullopt;

    auto const& camera   = client->getCamera();
    auto const  position = *camera.mPosition;
    auto const  forward  = *camera.mForward;
    auto const  fov      = camera.mFov;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)
        || !std::isfinite(forward.x) || !std::isfinite(forward.y) || !std::isfinite(forward.z)
        || !std::isfinite(fov)) {
        return std::nullopt;
    }

    constexpr float RadiansToDegrees = 57.2957795130823208768f;
    editing::model::CameraKeyframe key;
    key.position = {position.x, position.y, position.z};
    key.yaw      = std::atan2(-forward.x, forward.z) * RadiansToDegrees;
    key.pitch    = std::atan2(-forward.y, std::hypot(forward.x, forward.z)) * RadiansToDegrees;
    key.fov      = std::clamp(fov, 1.0f, 179.0f);
    return key;
}

void EditorController::reset() {
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
    mProjectTotalTicks = -1;
}

void EditorController::ensureProject(int totalTicks, std::string_view replayPath) {
    totalTicks = std::max(0, totalTicks);
    if (mProjectTotalTicks == totalTicks && mProject.projectPath == replayPath) return;

    mProject             = {};
    mProject.projectPath = std::string(replayPath);
    mProject.totalTicks  = totalTicks;
    editing::CameraBindingOps::addFreeCamera(mProject, "Camera 1");
    mProject.worldActor.segments.push_back({"worldActor", 0, totalTicks, 0});
    mCommandStack.clear();
    mPreviewCameraId.reset();
    mProjectTotalTicks = totalTicks;
    publishCameraTimeline();
}

void EditorController::applyEditorAction(EditorAction const& action) {
    using namespace editing::command;

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
        mCommandStack.push(
            CommandFactory::createAddCameraKeyframe(action.id, action.tick, captureCameraKeyframe()),
            mProject
        );
        break;
    case EditorActionType::MoveCameraKeyframe:
        mCommandStack.push(
            CommandFactory::createMoveCameraKeyframe(action.id, action.secondaryId, action.tick),
            mProject
        );
        break;
    case EditorActionType::DeleteCameraKeyframe:
        mCommandStack.push(CommandFactory::createDeleteCameraKeyframe(action.id, action.secondaryId), mProject);
        break;
    case EditorActionType::SetKeyframeEasing:
        mCommandStack.push(
            CommandFactory::createSetKeyframeEasing(
                action.id,
                action.secondaryId,
                static_cast<editing::model::EasingType>(action.kind)
            ),
            mProject
        );
        break;
    case EditorActionType::DeleteCamera:
        mCommandStack.push(CommandFactory::createDeleteCamera(action.id), mProject);
        break;
    case EditorActionType::UnbindCamera:
        mCommandStack.push(CommandFactory::createUnbindCamera(action.id), mProject);
        break;
    case EditorActionType::SetCameraKind:
        mCommandStack.push(
            CommandFactory::createSetCameraKind(action.id, static_cast<editing::model::CameraKind>(action.kind)),
            mProject
        );
        break;
    case EditorActionType::CreateBindingCamera:
        mCommandStack.push(CommandFactory::createCreateBindingCamera(action.id, action.name), mProject);
        break;
    case EditorActionType::SetSubActorDetails:
        mCommandStack.push(CommandFactory::createSetSubActorDetails(action.id, action.details), mProject);
        break;
    case EditorActionType::SetPreviewCamera:
        if (std::ranges::any_of(mProject.cameras, [&](auto const& camera) { return camera.id == action.id; })) {
            mPreviewCameraId = action.id;
        }
        break;
    case EditorActionType::ClearPreviewCamera:
        mPreviewCameraId.reset();
        break;
    default:
        break;
    }

    if (mPreviewCameraId
        && !std::ranges::any_of(mProject.cameras, [&](auto const& camera) { return camera.id == *mPreviewCameraId; })) {
        mPreviewCameraId.reset();
    }
    publishCameraTimeline();
}

void EditorController::publishState(bool hudVisible) {
    auto& session = functions::ReplaySession::getInstance();

    EditorState state;
    state.replayVisible = session.isActive() && session.hasJoinedReplayWorld();
    state.editorVisible = session.isActive();
    state.hudVisible    = hudVisible;
    state.paused        = session.isPaused();
    state.playbackSpeed = session.getPlaybackSpeed();
    state.currentTick   = std::max(0, session.getCurrentTick());
    state.totalTicks    = std::max(0, session.getTotalTicks());
    if (!state.editorVisible) mActiveReplayPath.clear();
    ensureProject(state.totalTicks, mActiveReplayPath);
    mProject.currentTick             = state.currentTick;
    mProject.playing                 = !state.paused;
    mProject.playbackSpeed           = state.playbackSpeed;
    state.project                    = std::make_shared<editing::model::EditorStateExt>(mProject);
    state.canUndo                    = mCommandStack.canUndo();
    state.canRedo                    = mCommandStack.canRedo();
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
    auto replays       = screen::ReplayBrowser::loadReplays();
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

    auto& session = functions::ReplaySession::getInstance();

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
        case EditorActionType::SkipToStart:
            session.requestSeek(0);
            break;
        case EditorActionType::SkipToEnd:
            session.requestSeek(session.getTotalTicks());
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
            if (mExportDriver) (void)mExportDriver->start(std::move(settings), mProject);
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
                auto replay = action.path.empty() ? screen::ReplayBrowser::findReplay(action.replayId)
                                                  : screen::ReplayBrowser::findReplay(action.path.string());
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
                if (screen::ReplayBrowser::importReplay(action.path, mBrowserError)) refreshBrowser();
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
                    auto replay = screen::ReplayBrowser::findReplay(entry->path.string());
                    if (!replay || !screen::ReplayBrowser::deleteReplay(*replay, mBrowserError)) break;
                    changed = true;
                }
                if (changed) refreshBrowser();
            });
            break;
        case EditorActionType::RenameReplay:
            runBrowserOperation(ReplayBrowserOperation::RenamingReplay, hudVisible, [&] {
                auto const* entry  = findBrowserEntry(action.replayId);
                auto        replay = entry ? screen::ReplayBrowser::findReplay(entry->path.string()) : std::nullopt;
                if (!replay) {
                    mBrowserError = "playback.replayBrowser.error.fileNotFound"_tr();
                } else if (screen::ReplayBrowser::renameReplay(*replay, action.name, mBrowserError)) {
                    refreshBrowser();
                }
            });
            break;
        case EditorActionType::ShowReplayInFolder:
            runBrowserOperation(ReplayBrowserOperation::ShowingInFolder, hudVisible, [&] {
                auto const* entry  = findBrowserEntry(action.replayId);
                auto        replay = entry ? screen::ReplayBrowser::findReplay(entry->path.string()) : std::nullopt;
                if (!replay) {
                    mBrowserError = "playback.replayBrowser.error.fileNotFound"_tr();
                } else if (!screen::ReplayBrowser::showInFolder(*replay)) {
                    mBrowserError = "playback.replayBrowser.error.showInFolderFailed"_tr();
                } else {
                    mBrowserError.clear();
                }
            });
            break;
        case EditorActionType::ClearReplayBrowserError:
            mBrowserError.clear();
            break;
        }
    }

    if (mExportDriver) mExportDriver->tick();
    publishState(hudVisible);
}

} // namespace playback::editor
