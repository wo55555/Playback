#include "SequenceOps.h"

#include <algorithm>

namespace playback::state::editing::SequenceOps {
std::string splitAt(std::vector<model::SequenceSegment>& segments, int atTick) {
    auto it = std::find_if(segments.begin(), segments.end(), [atTick](const auto& segment) {
        return atTick > segment.startTick && atTick < segment.endTick && !segment.locked;
    });
    if (it == segments.end()) return {};
    auto right      = *it;
    right.id        = it->id + ".split." + std::to_string(atTick);
    right.startTick = atTick;
    it->endTick     = atTick;
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
bool trimSegment(
    std::vector<model::SequenceSegment>& segments,
    const std::string&                   segmentId,
    int                                  newStartTick,
    int                                  newEndTick
) {
    auto it =
        std::find_if(segments.begin(), segments.end(), [&](const auto& segment) { return segment.id == segmentId; });
    if (it == segments.end() || it->locked || newStartTick >= newEndTick
        || (it->startTick == newStartTick && it->endTick == newEndTick))
        return false;
    auto index = static_cast<size_t>(std::distance(segments.begin(), it));
    if ((index == 0 && newStartTick != 0) || (index + 1 == segments.size() && newEndTick != it->endTick)) return false;
    if (index && (segments[index - 1].locked || newStartTick <= segments[index - 1].startTick)) return false;
    if (index + 1 < segments.size() && (segments[index + 1].locked || newEndTick >= segments[index + 1].endTick))
        return false;
    if (index) segments[index - 1].endTick = newStartTick;
    if (index + 1 < segments.size()) segments[index + 1].startTick = newEndTick;
    it->startTick = newStartTick;
    it->endTick   = newEndTick;
    return true;
}
void bindCamera(model::SequenceSegment& segment, const std::string& cameraId) { segment.cameraId = cameraId; }
void clearDanglingRefs(std::vector<model::SequenceSegment>& segments, const std::string& removedCameraId) {
    for (auto& segment : segments)
        if (segment.cameraId == removedCameraId) segment.cameraId.clear();
}
} // namespace playback::state::editing::SequenceOps
