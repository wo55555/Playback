#pragma once

#include "playback/refactor/editor/models/CameraKeyframe.h"
#include "playback/refactor/editor/models/Track.h"
#include "playback/refactor/editor/Splitter.h"

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

private:
    void drawHeader();
    void drawRuler();
    void drawBody();
    void drawVideoTrack(int index, Rect rowArea);
    void drawCameraTrackRows();
    void drawMarkerRow();
    void drawVideoClipOnTrack(const Clip& c, Rect rowArea, int trackIndex);
    void drawKeyframeOnTrack(const CameraKeyframe& k, Rect rowArea);
    void drawMarkerOnRow(const Marker& m, Rect rowArea);
    void drawTransitionBetween(int trackIndex, const Transition& t, Rect rowArea);
    void drawPlayhead(Rect fullArea);
    void drawSelectionRect(Rect fullArea);

    // Interaction
    void handleRulerClick(Rect rulerArea);
    void handleClipClick(const Clip& c, Rect rowArea, int trackIndex, bool hovered);
    void handleKeyframeClick(const CameraKeyframe& k, Rect rowArea, bool hovered);
    void clipDragUpdate(Clip& c, Rect rowArea);
    void keyframeDragUpdate(CameraKeyframe& k, Rect rowArea);
    void handleSplitAtPlayhead();
    void handleRippleDelete();
    void handleCopy();
    void handlePaste();
    void handleTrimLeftToPlayhead();
    void handleTrimRightToPlayhead();

    // Commit drag operation to command stack on mouse release
    void commitDragOperation();

    // Context menus
    void drawClipContextMenu(const Clip& c, int trackIndex);
    void drawTrackContextMenu(int trackIndex);

    // Hit testing
    struct ClipHit {
        int trackIndex{};
        int clipIndex{};
        bool onLeftEdge{};
        bool onRightEdge{};
    };
    std::optional<ClipHit> hitTestClip(ImVec2 mousePos, Rect rowArea, int trackIndex);
    std::optional<int> hitTestKeyframe(ImVec2 mousePos, Rect rowArea, int trackIndex);

    // Zoom
    void onWheel(float deltaY);

    float mPixelsPerTick{0.1f};
    float mScrollX{0.0f};
    int   mPlayheadTick{};

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
    static constexpr float kLabelWidth = 60.0f;
    static constexpr float kRulerHeight = 24.0f;
    static constexpr float kVideoTrackMinH = 48.0f;
    static constexpr float kCameraTrackH = 24.0f;
    static constexpr float kMarkerTrackH = 20.0f;
};

} // namespace playback::refactor::editor