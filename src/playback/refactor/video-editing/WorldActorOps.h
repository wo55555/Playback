#pragma once

#include "playback/refactor/editor/models/WorldActor.h"

#include <string>

namespace playback::refactor::video_editing::WorldActorOps {
bool validateCoverage(const editor::WorldActor& worldActor, int totalTicks);
std::string splitAt(editor::WorldActor& worldActor, int atTick);
bool trimSegment(editor::WorldActor& worldActor, const std::string& segmentId, int newStartTick, int newEndTick, int totalTicks);
bool setSpeed(editor::WorldActor& worldActor, const std::string& segmentId, float speed);
bool rippleDelete(editor::WorldActor& worldActor, const std::string& segmentId, int totalTicks);
int mapTimelineToSourceTick(const editor::WorldActor& worldActor, int timelineTick);
}
