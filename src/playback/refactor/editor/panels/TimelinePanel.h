#pragma once

#include "playback/refactor/editor/models/CameraKeyframe.h"
#include "playback/refactor/editor/models/Track.h"
#include "playback/refactor/editor/Splitter.h"

#include <optional>
#include <string>

namespace playback::refactor::editor {

enum class DragType {
    None,
    MoveClip,       // dragging clip body → move
    TrimClipIn,     // dragging clip left edge → trim in
    TrimClipOut,    // dragging clip right edge → trim out
    MoveKeyframe,   // dragging keyframe
    MovePlayhead,   // scrubbing on ruler
    SelectRange     // shift+drag on ruler
};

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
    void handleSplitAtPlayhead();
    void handleRippleDelete();
    void handleTrimLeftToPlayhead();
    void handleTrimRightToPlayhead();
    void commitDragOperation();
    void onWheel(float deltaY);
    void adjustTimeScale(float multiplier);

    float mPixelsPerTick{0.25f};
    float mScrollX{0.0f};
    float mTrackListWidthRatio{0.30f};
    int   mPlayheadTick{};
    std::string mTrackSearch;
    bool mSnapEnabled{true};
    bool mVideoGroupOpen{true};
    bool mCameraGroupOpen{true};
    bool mMarkerGroupOpen{true};
    bool mRulerScrubbing{};

    // Drag state
    DragType mDragType{DragType::None};
    std::string mDragTargetId;
    int mDragTrackIndex{};
    int mDragStartTick{};
    int mDragOrigTick{};

    // Selection
    std::string mSelectedClipId;
    int mSelectedTrackIndex{-1};

    // Layout
    static constexpr float kLabelWidth = 0.0f;
    static constexpr float kRulerHeight = 28.0f;
    static constexpr float kVideoTrackMinH = 48.0f;
    static constexpr float kCameraTrackH = 24.0f;
    static constexpr float kMarkerTrackH = 20.0f;
};

} // namespace playback::refactor::editor
