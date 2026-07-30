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
    bool                     locked{};
    bool                     muted{};
    bool                     visible{true};
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
    float playbackSpeed{1.0f};

    // Camera tracks
    std::vector<CameraTrackExt> cameraTracks;
    int activeCameraIndex{};

    // Video tracks (multi-track per 04-video-editing)
    std::vector<Track> videoTracks;
    int activeVideoTrackIdx{};

    // Transitions
    std::vector<Transition> transitions;

    // Markers
    std::vector<Marker> markers;

    // Performance
    float fps{60.0f};
    size_t memoryUsageBytes{};
};

} // namespace playback::refactor::editor
