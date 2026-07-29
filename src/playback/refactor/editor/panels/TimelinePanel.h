#pragma once

#include "playback/refactor/editor/models/CameraKeyframe.h"
#include "playback/refactor/editor/models/Track.h"
#include "playback/refactor/editor/Splitter.h"

#include <string>

namespace playback::refactor::editor {

class TimelinePanel {
public:
    void draw();

private:
    void drawHeader();
    void drawBody();
    void drawTrack(const TrackDescriptor& track, Rect rowArea);
    void drawVideoClip(const Clip& c, Rect rowArea);
    void drawKeyframe(const CameraKeyframe& k, Rect rowArea);
    void drawMarker(const Marker& m, Rect rowArea);
    void drawTransition(const Transition& t, Rect area);

    // Drag handlers
    void handleScrubDrag(Rect headerArea);
    void handleClipDrag(Clip& c, Rect rowArea);
    void handleKeyframeDrag(CameraKeyframe& k, Rect rowArea);

    // Zoom
    void onWheel(float deltaY);

    float mPixelsPerTick{0.1f}; // 0.02 ~ 2.0
    float mScrollX{0.0f};
    int   mPlayheadTick{};
    std::string mDragTargetId;
};

} // namespace playback::refactor::editor