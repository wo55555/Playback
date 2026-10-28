#pragma once

#include "playback/state/editing/models/SequenceSegment.h"

#include <cstddef>
#include <string>
#include <vector>

namespace playback::state::editing::SequenceOps {
std::string splitAt(std::vector<model::SequenceSegment>& segments, int atTick);
bool        deleteSegment(std::vector<model::SequenceSegment>& segments, size_t index, int totalTicks);
// Absorbs the neighbour into this segment (withNext) or this segment into the previous one.
bool        mergeWithNeighbour(std::vector<model::SequenceSegment>& segments, const std::string& segmentId, bool withNext);
bool        trimSegment(
           std::vector<model::SequenceSegment>& segments,
           const std::string&                   segmentId,
           int                                  newStartTick,
           int                                  newEndTick
       );
void bindCamera(model::SequenceSegment& segment, const std::string& cameraId);
void clearDanglingRefs(std::vector<model::SequenceSegment>& segments, const std::string& removedCameraId);
} // namespace playback::state::editing::SequenceOps
