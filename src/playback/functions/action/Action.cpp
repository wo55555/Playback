#include "Action.h"

#include "playback/functions/replay/ReplaySession.h"

namespace playback::functions {

// ActionNextTick
void ActionNextTick::handle(functions::ReplaySession& session, BinaryStream&) { session.handleNextTick(); }

} // namespace playback::functions
