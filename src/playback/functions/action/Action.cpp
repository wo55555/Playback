#include "Action.h"

#include "playback/functions/io/AsyncReplaySaver.h"
#include "playback/functions/replay/ReplaySession.h"

namespace playback::functions {

// ActionNextTick
void ActionNextTick::handle(functions::ReplaySession& session, PlaybackBuffer&) { session.handleNextTick(); }

// ActionLevelChunkCached
void ActionLevelChunkCached::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleLevelChunkCached(data.getVarInt().value());
}

} // namespace playback::functions
