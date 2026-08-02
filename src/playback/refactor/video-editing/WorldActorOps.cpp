#include "WorldActorOps.h"

#include <algorithm>
#include <cmath>

namespace playback::refactor::video_editing::WorldActorOps {
bool validateCoverage(const editor::WorldActor& worldActor, int totalTicks) {
    if (worldActor.segments.empty() || worldActor.segments.front().startTick != 0 || worldActor.segments.back().endTick != totalTicks) return false;
    for (size_t i = 0; i < worldActor.segments.size(); ++i) if (worldActor.segments[i].startTick >= worldActor.segments[i].endTick || (i && worldActor.segments[i - 1].endTick != worldActor.segments[i].startTick)) return false;
    return true;
}
std::string splitAt(editor::WorldActor& worldActor, int atTick) {
    auto& segments = worldActor.segments;
    auto it = std::find_if(segments.begin(), segments.end(), [atTick](const auto& segment) { return atTick > segment.startTick && atTick < segment.endTick && !segment.locked; });
    if (it == segments.end()) return {};
    auto right = *it;
    right.id = it->id + ".split." + std::to_string(atTick);
    right.startTick = atTick;
    right.sourceTick = it->sourceTick + static_cast<int>(std::floor((atTick - it->startTick) * it->speed));
    it->endTick = atTick;
    segments.insert(std::next(it), right);
    return right.id;
}
bool trimSegment(editor::WorldActor& worldActor, const std::string& id, int start, int end, int totalTicks) {
    auto& segments = worldActor.segments;
    auto it = std::find_if(segments.begin(), segments.end(), [&](const auto& segment) { return segment.id == id; });
    if (it == segments.end() || it->locked || start >= end) return false;
    size_t index = std::distance(segments.begin(), it);
    if ((index == 0 && start != 0) || (index + 1 == segments.size() && end != totalTicks)) return false;
    if (index && (segments[index - 1].locked || start <= segments[index - 1].startTick)) return false;
    if (index + 1 < segments.size() && (segments[index + 1].locked || end >= segments[index + 1].endTick)) return false;
    if (index) segments[index - 1].endTick = start;
    if (index + 1 < segments.size()) segments[index + 1].startTick = end;
    it->startTick = start; it->endTick = end;
    return true;
}
bool setSpeed(editor::WorldActor& worldActor, const std::string& id, float speed) { auto it = std::find_if(worldActor.segments.begin(), worldActor.segments.end(), [&](const auto& segment) { return segment.id == id; }); if (it == worldActor.segments.end() || it->locked || speed <= 0.0f) return false; it->speed = speed; return true; }
bool rippleDelete(editor::WorldActor& worldActor, const std::string& id, int totalTicks) { auto& segments = worldActor.segments; auto it = std::find_if(segments.begin(), segments.end(), [&](const auto& segment) { return segment.id == id; }); if (it == segments.end()) return false; return [&] { auto index = static_cast<size_t>(std::distance(segments.begin(), it)); if (segments.size() <= 1 || it->locked) return false; if (index == 0) segments[1].startTick = 0; else if (index + 1 == segments.size()) segments[index - 1].endTick = totalTicks; else segments[index - 1].endTick = segments[index + 1].startTick; segments.erase(segments.begin() + index); return true; }(); }
int mapTimelineToSourceTick(const editor::WorldActor& worldActor, int tick) { auto it = std::find_if(worldActor.segments.begin(), worldActor.segments.end(), [tick](const auto& segment) { return tick >= segment.startTick && tick < segment.endTick; }); return it == worldActor.segments.end() ? -1 : it->sourceTick + static_cast<int>(std::floor((tick - it->startTick) * it->speed)); }
}
