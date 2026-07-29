#pragma once

#include "CameraKeyframe.h"

#include <string>
#include <vector>

namespace playback::refactor::editor {

// A clip on the video track (V0)
struct Clip {
    std::string id;
    std::string name;
    int         startTick{};
    int         endTick{};
    std::string sourceFile;
};

// A transition between two clips
struct Transition {
    std::string id;
    std::string type;  // "CrossDissolve", "FadeToBlack", etc.
    int         durationTicks{};
    int         startTick{};
};

// A camera track (Cn)
struct CameraTrack {
    std::string              id;
    std::string              name;
    bool                     active{};
    bool                     locked{};
    bool                     muted{};
    bool                     visible{true};
    std::vector<CameraKeyframe> keyframes;
};

// A marker on the marker track (M)
struct Marker {
    std::string id;
    std::string label;
    int         tick{};
};

// Track type for TimelinePanel
enum class TrackType {
    Video,      // V0
    Camera,     // Cn
    Markers     // M
};

// Generic track descriptor for timeline rendering
struct TrackDescriptor {
    TrackType type{TrackType::Camera};
    std::string id;
    std::string name;
    bool        active{};
    bool        locked{};
    bool        muted{};
    bool        visible{true};
};

} // namespace playback::refactor::editor