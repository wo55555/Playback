#pragma once

#include "CameraEntity.h"
#include "Track.h"

#include <string>
#include <vector>

namespace playback::state::editing::model {

struct Scene {
    std::string id;
    std::string name;

    std::vector<CameraEntity> cameras;
    std::vector<Marker>       markers;
    SpeedTrack                speedTrack;
    FreezeTrack               freezeTrack;
    TimelapseTrack            timelapseTrack;
    std::vector<AudioTrack>   audioTracks;

    int exportInTick{-1};
    int exportOutTick{-1};
};

struct EditorStateExt {
    std::string projectName;
    std::string projectPath;

    int   currentTick{};
    int   totalTicks{};
    bool  playing{};
    float playbackSpeed{1.0f};

    std::vector<Scene> scenes;
    std::string        activeSceneId;

    // Live working copy of the active scene.
    std::vector<CameraEntity> cameras;
    std::vector<Marker>       markers;
    SpeedTrack                speedTrack;
    FreezeTrack               freezeTrack;
    TimelapseTrack            timelapseTrack;
    std::vector<AudioTrack>   audioTracks;
    int                       exportInTick{-1};
    int                       exportOutTick{-1};

    std::vector<Transition> transitions;

    float  fps{60.0f};
    size_t memoryUsageBytes{};
};

} // namespace playback::state::editing::model
