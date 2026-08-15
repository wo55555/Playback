#pragma once

#include "playback/state/EditorAction.h"
#include "playback/state/editing/models/TrackTreeModel.h"

#include <string>

namespace playback::editor::ui {

class TimelinePanel {
public:
    void draw(bool allowInput);

    void seekTo(int tick);
    void seekRelative(int tickDelta);
    void seekAdjacentEditPoint(bool forward);
    bool addKeyframeAtPlayhead();
    bool splitAtPlayhead();
    bool deleteSelection();
    void zoomIn();
    void zoomOut();
    void resetZoom();

    [[nodiscard]] float trackListWidthRatio() const { return mTrackListWidthRatio; }
    [[nodiscard]] float zoomScale() const { return mZoomScale; }
    [[nodiscard]] float horizontalScroll() const { return mScrollX; }
    void                setViewPreferences(float trackListWidthRatio, float zoomScale, float horizontalScroll);

private:
    void submitSeek(int tick);
    void submitEdit(playback::state::EditorAction action);

    state::editing::model::TrackTreeModel mTrackTree;
    float                          mZoomScale{1.0f};
    float                          mScrollX{};
    float                          mTrackListWidthRatio{0.30f};
    int                            mPendingSeekTick{-1};
    int                            mRulerDragTick{-1};
    std::string                    mTrackSearch;
    bool                           mSnapEnabled{true};
    bool                           mCamerasExpanded{true};
    std::string                    mDraggingSegmentId;
    bool                           mDraggingStart{};
    bool                           mDraggingPlayhead{};
    int                            mDragStartTick{};
    int                            mDragEndTick{};
    std::string                    mDraggingKeyframeCameraId;
    float                          mDraggingKeyframeStartMouseX{};
    int                            mDraggingKeyframeStartTick{};
    int                            mDraggingKeyframeTick{};
    bool                           mDraggingKeyframeMoved{};
};

} // namespace playback::editor::ui
