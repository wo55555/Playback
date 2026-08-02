#pragma once

#include "playback/editor/editing/models/WorldActor.h"

#include <string>

namespace playback::editor::editing::WorldActorOps {
bool validateCoverage(const model::WorldActor& worldActor, int totalTicks);
std::string splitAt(model::WorldActor& worldActor, int atTick);
bool trimSegment(model::WorldActor& worldActor, const std::string& segmentId, int newStartTick, int newEndTick, int totalTicks);
bool setSpeed(model::WorldActor& worldActor, const std::string& segmentId, float speed);
bool rippleDelete(model::WorldActor& worldActor, const std::string& segmentId, int totalTicks);
int mapTimelineToSourceTick(const model::WorldActor& worldActor, int timelineTick);
}
