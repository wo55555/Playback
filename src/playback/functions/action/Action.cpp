#include "Action.h"

#include "playback/functions/io/AsyncReplaySaver.h"
#include "playback/functions/replay/ReplaySession.h"

namespace playback::functions {

void ActionNextTick::handle(functions::ReplaySession& session, PlaybackBuffer&) { session.handleNextTick(); }

void ActionSnapshotContext::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleSnapshotContext(readSnapshotContext(data));
}

void ActionCreateLocalPlayer::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleCreateLocalPlayer(data);
}

void ActionLevelChunkCached::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleLevelChunkCached(data.getVarInt().value());
}

void ActionSubChunkCached::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleSubChunkCached(data.getVarInt().value());
}

void ActionConfigurationPacket::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleConfigurationPacket(data);
}

void ActionGamePacket::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleGamePacket(data);
}

void ActionMoveEntities::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleMoveEntities(data);
}

} // namespace playback::functions
