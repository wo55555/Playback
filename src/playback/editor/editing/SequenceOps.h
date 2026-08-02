#pragma once

#include "playback/editor/editing/models/SequenceSegment.h"

#include <cstddef>
#include <string>
#include <vector>

namespace playback::editor::editing::SequenceOps {
const model::SequenceSegment* findSegmentAt(const std::vector<model::SequenceSegment>& segments, int tick);
bool validateCoverage(const std::vector<model::SequenceSegment>& segments, int totalTicks);
std::string splitAt(std::vector<model::SequenceSegment>& segments, int atTick);
bool deleteSegment(std::vector<model::SequenceSegment>& segments, size_t index, int totalTicks);
bool trimSegment(std::vector<model::SequenceSegment>& segments, const std::string& segmentId, int newStartTick, int newEndTick);
void bindCamera(model::SequenceSegment& segment, const std::string& cameraId);
void clearDanglingRefs(std::vector<model::SequenceSegment>& segments, const std::string& removedCameraId);
}
