#pragma once

#include "playback/state/editing/models/WorldActor.h"

#include <string>

namespace playback::state::editing::WorldActorOps {
std::string splitAt(model::WorldActor& worldActor, int atTick);
bool        trimSegment(
           model::WorldActor& worldActor,
           const std::string& segmentId,
           int                newStartTick,
           int                newEndTick,
           int                totalTicks
       );
bool setSpeed(model::WorldActor& worldActor, const std::string& segmentId, float speed);
bool rippleDelete(model::WorldActor& worldActor, const std::string& segmentId, int totalTicks);
} // namespace playback::state::editing::WorldActorOps
