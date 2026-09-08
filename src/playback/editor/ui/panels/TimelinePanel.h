#pragma once

#include "playback/editor/ui/PanelContext.h"
#include "playback/editor/ui/components/Animator.h"
#include "playback/editor/ui/components/TrackTreeModel.h"
#include "playback/state/EditorAction.h"

#include <string>

namespace playback::editor::ui {

class TimelinePanel {
public:
    void draw(PanelContext const& ctx, bool allowInput);

    void seekTo(PanelContext const& ctx, int tick);
    void seekRelative(PanelContext const& ctx, int tickDelta);
    void seekAdjacentEditPoint(PanelContext const& ctx, bool forward);
    bool addKeyframeAtPlayhead(PanelContext const& ctx);
    bool deleteSelection(PanelContext const& ctx);
    void zoomIn();
    void zoomOut();
    void resetZoom();

    [[nodiscard]] float trackListWidthRatio() const { return mTrackListWidthRatio; }
    [[nodiscard]] float zoomScale() const { return mZoomScale; }
    [[nodiscard]] float horizontalScroll() const { return mScrollX; }
    void                setViewPreferences(float trackListWidthRatio, float zoomScale, float horizontalScroll);

private:
    void submitSeek(PanelContext const& ctx, int tick);

    TrackTreeModel mTrackTree;
    Animator       mAnimator;
    float          mZoomScale{1.0f};
    float          mScrollX{};
    float          mScrollY{};
    float          mTrackListWidthRatio{0.30f};
    int            mPendingSeekTick{-1};
    int            mRulerDragTick{-1};
    std::string    mTrackSearch;
    bool           mSnapEnabled{true};
    bool           mCamerasExpanded{true};
    bool           mDraggingPlayhead{};
    std::string    mDraggingKeyframeCameraId;
    float          mDraggingKeyframeStartMouseX{};
    int            mDraggingKeyframeStartTick{};
    int            mDraggingKeyframeTick{};
    bool           mDraggingKeyframeMoved{};
};

} // namespace playback::editor::ui
