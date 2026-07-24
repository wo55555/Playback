#include "EditorController.h"

#include "playback/functions/replay/ReplaySession.h"

#include <algorithm>

namespace playback::editor {

EditorController::EditorController(EditorContext& context) : mContext(context) {}

void EditorController::tick(bool hudVisible) {
    auto& session = functions::ReplaySession::getInstance();

    for (auto const action : mContext.takeActions()) {
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
        }
    }

    EditorState state;
    state.replayVisible = session.isActive() && session.hasJoinedReplayWorld();
    state.hudVisible    = hudVisible;
    state.paused        = session.isPaused();
    state.playbackSpeed = session.getPlaybackSpeed();
    state.currentTick   = std::max(0, session.getCurrentTick());
    state.totalTicks    = std::max(0, session.getTotalTicks());
    mContext.publish(state);
}

} // namespace playback::editor
