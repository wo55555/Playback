#include "Action.h"

namespace playback::functions {

// ActionNextTick
void ActionNextTick::handle(functions::ReplaySession replaySession) const { replaySession.handleNextTick(); }

} // namespace playback::functions
