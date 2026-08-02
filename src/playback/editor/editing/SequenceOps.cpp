#include "SequenceOps.h"

#include <algorithm>

namespace playback::editor::editing::SequenceOps {
const model::SequenceSegment* findSegmentAt(const std::vector<model::SequenceSegment>& segments, int tick) {
    auto it = std::upper_bound(segments.begin(), segments.end(), tick, [](int value, const auto& segment) { return value < segment.startTick; });
    if (it == segments.begin()) return nullptr;
    --it;
    return tick >= it->startTick && tick < it->endTick ? &*it : nullptr;
}
bool validateCoverage(const std::vector<model::SequenceSegment>& segments, int totalTicks) {
    if (segments.empty() || segments.front().startTick != 0 || segments.back().endTick != totalTicks) return false;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (segments[i].startTick >= segments[i].endTick) return false;
        if (i && segments[i - 1].endTick != segments[i].startTick) return false;
    }
    return true;
}
std::string splitAt(std::vector<model::SequenceSegment>& segments, int atTick) {
    auto it = std::find_if(segments.begin(), segments.end(), [atTick](const auto& segment) { return atTick > segment.startTick && atTick < segment.endTick && !segment.locked; });
    if (it == segments.end()) return {};
    auto right = *it;
    right.id = it->id + ".split." + std::to_string(atTick);
    right.startTick = atTick;
    it->endTick = atTick;
    segments.insert(std::next(it), right);
    return right.id;
}
bool deleteSegment(std::vector<model::SequenceSegment>& segments, size_t index, int totalTicks) {
    if (segments.size() <= 1 || index >= segments.size() || segments[index].locked) return false;
    if (index == 0) segments[1].startTick = 0;
    else if (index + 1 == segments.size()) segments[index - 1].endTick = totalTicks;
    else segments[index - 1].endTick = segments[index + 1].startTick;
    segments.erase(segments.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}
bool trimSegment(std::vector<model::SequenceSegment>& segments, const std::string& segmentId, int newStartTick, int newEndTick) {
    auto it = std::find_if(segments.begin(), segments.end(), [&](const auto& segment) { return segment.id == segmentId; });
    if (it == segments.end() || it->locked || newStartTick >= newEndTick) return false;
    auto index = static_cast<size_t>(std::distance(segments.begin(), it));
    if ((index == 0 && newStartTick != 0) || (index + 1 == segments.size() && newEndTick != it->endTick)) return false;
    if (index && (segments[index - 1].locked || newStartTick <= segments[index - 1].startTick)) return false;
    if (index + 1 < segments.size() && (segments[index + 1].locked || newEndTick >= segments[index + 1].endTick)) return false;
    if (index) segments[index - 1].endTick = newStartTick;
    if (index + 1 < segments.size()) segments[index + 1].startTick = newEndTick;
    it->startTick = newStartTick;
    it->endTick = newEndTick;
    return true;
}
void bindCamera(model::SequenceSegment& segment, const std::string& cameraId) { segment.cameraId = cameraId; }
void clearDanglingRefs(std::vector<model::SequenceSegment>& segments, const std::string& removedCameraId) { for (auto& segment : segments) if (segment.cameraId == removedCameraId) segment.cameraId.clear(); }
}
