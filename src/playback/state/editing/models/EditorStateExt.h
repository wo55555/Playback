#pragma once

#include "CameraEntity.h"
#include "SequenceSegment.h"
#include "Track.h"
#include "WorldActor.h"

#include <string>
#include <vector>

namespace playback::state::editing::model {

struct EditorStateExt {
    std::string projectName;
    std::string projectPath;

    int   currentTick{};
    int   totalTicks{};
    bool  playing{};
    float playbackSpeed{1.0f};

    std::vector<SequenceSegment> sequence;
    WorldActor                   worldActor;
    std::vector<CameraEntity>    cameras;

    std::vector<Track> videoTracks;
    int                activeVideoTrackIdx{};

    std::vector<Transition> transitions;

    std::vector<Marker> markers;

    float  fps{60.0f};
    size_t memoryUsageBytes{};
};

} // namespace playback::state::editing::model
