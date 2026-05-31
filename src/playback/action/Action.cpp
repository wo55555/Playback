#include "Action.h"

namespace playback::action {

// ActionNextTick
void ActionNextTick::handle(functions::ReplayServer replayServer) const { replayServer.handleNextTick(); }

// ActionGamePacket
void ActionGamePacket::handle(functions::ReplayServer replayServer) const { replayServer.handleGamePacket(); }

} // namespace playback::action
