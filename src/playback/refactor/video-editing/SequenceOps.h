#pragma once

#include "playback/refactor/editor/models/SequenceSegment.h"

#include <cstddef>
#include <string>
#include <vector>

namespace playback::refactor::video_editing::SequenceOps {
const editor::SequenceSegment* findSegmentAt(const std::vector<editor::SequenceSegment>& segments, int tick);
bool validateCoverage(const std::vector<editor::SequenceSegment>& segments, int totalTicks);
std::string splitAt(std::vector<editor::SequenceSegment>& segments, int atTick);
bool deleteSegment(std::vector<editor::SequenceSegment>& segments, size_t index, int totalTicks);
bool trimSegment(std::vector<editor::SequenceSegment>& segments, const std::string& segmentId, int newStartTick, int newEndTick);
void bindCamera(editor::SequenceSegment& segment, const std::string& cameraId);
void clearDanglingRefs(std::vector<editor::SequenceSegment>& segments, const std::string& removedCameraId);
}
