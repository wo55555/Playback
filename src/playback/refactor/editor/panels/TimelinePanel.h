#pragma once

#include "playback/refactor/editor/Splitter.h"
#include "playback/refactor/editor/models/TrackTreeModel.h"

#include <string>

namespace playback::refactor::editor {

class TimelinePanel {
public:
    void draw();
    [[nodiscard]] float trackListWidthRatio() const { return mTrackListWidthRatio; }
    [[nodiscard]] float pixelsPerTick() const { return mPixelsPerTick; }
    [[nodiscard]] float horizontalScroll() const { return mScrollX; }
    void setViewPreferences(float trackListWidthRatio, float pixelsPerTick, float horizontalScroll);

private:
    void drawHeader();
    void drawTrackList();
    void drawTransportControls();
    void drawRuler();
    void drawBody();
    void drawPlayhead(Rect fullArea);
    void handleRulerClick(Rect rulerArea);
    void onWheel(float deltaY);
    void adjustTimeScale(float multiplier);

    float mPixelsPerTick{0.25f};
    float mScrollX{};
    float mTrackListWidthRatio{0.30f};
    int mPlayheadTick{};
    std::string mTrackSearch;
    bool mSnapEnabled{true};
    bool mRulerScrubbing{};
    int mPendingSeekTick{-1};
    TrackTreeModel mTrackTree;

    static constexpr float kRulerHeight = 28.0f;
};

} // namespace playback::refactor::editor
