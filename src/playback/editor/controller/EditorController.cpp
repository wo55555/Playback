#include "EditorController.h"

#include "playback/editor/editing/commands/CameraCommands.h"
#include "playback/editor/editing/commands/CommandFactory.h"
#include "playback/functions/replay/ReplaySession.h"
#include "playback/screen/ReplayBrowser.h"

#include <algorithm>
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

} // namespace

EditorController::EditorController(EditorContext& context)
: mContext(context),
  mBrowserSnapshot(std::make_shared<ReplayBrowserSnapshot>()) {}

void EditorController::reset() {
    mBrowserVisible   = false;
    mBrowserOperation = ReplayBrowserOperation::None;
    mBrowserError.clear();
    mBrowserSnapshot = std::make_shared<ReplayBrowserSnapshot>();
    mProject         = {};
    mCommandStack.clear();
    mProjectTotalTicks = -1;
}

void EditorController::ensureProject(int totalTicks) {
    totalTicks = std::max(0, totalTicks);
    if (mProjectTotalTicks == totalTicks) return;

    mProject            = {};
    mProject.totalTicks = totalTicks;
    mProject.sequence.push_back({"sequence", 0, totalTicks});
    mProject.worldActor.segments.push_back({"worldActor", 0, totalTicks, 0});
    mCommandStack.clear();
    mProjectTotalTicks = totalTicks;
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
        mCommandStack.push(CommandFactory::createAddCameraKeyframe(action.id, action.tick), mProject);
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
    default:
        break;
    }
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
    ensureProject(state.totalTicks);
    mProject.currentTick             = state.currentTick;
    mProject.playing                 = !state.paused;
    mProject.playbackSpeed           = state.playbackSpeed;
    state.project                    = std::make_shared<editing::model::EditorStateExt>(mProject);
    state.canUndo                    = mCommandStack.canUndo();
    state.canRedo                    = mCommandStack.canRedo();
    state.capabilities.cameraEditing = state.editorVisible;
    state.capabilities.videoEditing  = state.editorVisible;
    state.browser.visible            = mBrowserVisible;
    state.browser.operation          = mBrowserOperation;
    state.browser.error              = mBrowserError;
    state.browser.snapshot           = mBrowserSnapshot;
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
    auto& session = functions::ReplaySession::getInstance();

    for (auto const& action : mContext.takeActions()) {
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
            session.requestStop();
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
                    mBrowserError = "Replay file no longer exists";
                } else if (!replay->canOpen) {
                    mBrowserError = replay->problem.empty() ? "Replay archive is invalid" : replay->problem;
                } else if (!session.start(replay->path)) {
                    mBrowserError = "Failed to start replay session";
                } else {
                    mBrowserVisible = false;
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
                        mBrowserError = "Replay file no longer exists";
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
                    mBrowserError = "Replay file no longer exists";
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
                    mBrowserError = "Replay file no longer exists";
                } else if (!screen::ReplayBrowser::showInFolder(*replay)) {
                    mBrowserError = "Unable to show replay in File Explorer";
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

    publishState(hudVisible);
}

} // namespace playback::editor
