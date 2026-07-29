#pragma once

#include "CameraKeyframe.h"
#include "Track.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace playback::refactor::editor {

struct CameraTrackExt {
    std::string              id;
    std::string              name;
    bool                     active{};
    std::vector<CameraKeyframe> keyframes;
};

struct EditorStateExt {
    // Project info
    std::string projectName;
    std::string projectPath;

    // Timeline
    int currentTick{};
    int totalTicks{};
    bool playing{};

    // Camera tracks
    std::vector<CameraTrackExt> cameraTracks;
    int activeCameraIndex{};

    // Video clips
    std::vector<Clip> videoClips;

    // Markers
    std::vector<Marker> markers;

    // Transitions
    std::vector<Transition> transitions;

    // Performance
    float fps{60.0f};
    size_t memoryUsageBytes{};
};

} // namespace playback::refactor::editor