#pragma once

#include "playback/editor/ui/PanelContext.h"
#include "playback/editor/ui/components/Animator.h"
#include "playback/editor/ui/components/TimelineScale.h"
#include "playback/editor/ui/components/TrackTreeModel.h"
#include "playback/state/EditorAction.h"

#include "imgui.h"

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

    [[nodiscard]] bool isSnapEnabled() const { return mSnapEnabled; }
    void               setSnapEnabled(bool enabled) { mSnapEnabled = enabled; }
    void               toggleSnap() { mSnapEnabled = !mSnapEnabled; }

private:
    struct Layout {
        ImVec2 fullMin{};
        ImVec2 fullMax{};
        float  titleBottom{};
        float  rulerTop{};
        float  headerHeight{};
        float  bodyTop{};
        float  bodyBottom{};
        float  listWidth{};
        float  canvasLeft{};
        float  canvasWidth{};
    };

    void submitSeek(PanelContext const& ctx, int tick);

    void drawTitleRow(PanelContext const& ctx, Layout const& layout, int displayTick, bool allowInput);
    void drawTrackHeaders(PanelContext const& ctx, Layout const& layout, bool allowInput);
    void drawRuler(
        PanelContext const&  ctx,
        Layout const&        layout,
        TimelineScale const& scale,
        int                  displayTick
    ); // Drawn after the track rows so the row bands do not cover it.
    void drawTrackGrid(PanelContext const& ctx, Layout const& layout, TimelineScale const& scale);
    void drawRangeBar(PanelContext const& ctx, Layout const& layout, bool allowInput);
    void drawTitleLabel(PanelContext const& ctx, float height);
    void drawTransportGroup(PanelContext const& ctx, int displayTick, float width);

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
    // Range-bar drag: 0 none, 1 left handle, 2 right handle, 3 whole window.
    int   mRangeDragMode{};
    float mRangeDragOriginX{};
    float mRangeDragOriginScroll{};
    float mRangeDragOriginZoom{1.0f};
};

} // namespace playback::editor::ui
