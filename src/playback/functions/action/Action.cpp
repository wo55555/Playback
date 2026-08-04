#include "Action.h"

#include "playback/functions/io/AsyncReplaySaver.h"
#include "playback/functions/replay/ReplaySession.h"

namespace playback::functions {

// ActionNextTick
void ActionNextTick::handle(functions::ReplaySession& session, PlaybackBuffer&) { session.handleNextTick(); }

// ActionSnapshotContext
void ActionSnapshotContext::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleSnapshotContext(readSnapshotContext(data));
}

// ActionCreateLocalPlayer
void ActionCreateLocalPlayer::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleCreateLocalPlayer(data);
}

// ActionLevelChunkCached
void ActionLevelChunkCached::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleLevelChunkCached(data.getVarInt().value());
}

// ActionSubChunkCached
void ActionSubChunkCached::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleSubChunkCached(data.getVarInt().value());
}

// ActionConfigurationPacket
void ActionConfigurationPacket::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleConfigurationPacket(data);
}

// ActionGamePacket
void ActionGamePacket::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleGamePacket(data);
}

// ActionMoveEntities
void ActionMoveEntities::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleMoveEntities(data);
}

} // namespace playback::functions
