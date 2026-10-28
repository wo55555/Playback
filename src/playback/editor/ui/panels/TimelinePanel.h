#pragma once

#include "playback/editor/ui/PanelContext.h"
#include "playback/editor/ui/components/Animator.h"
#include "playback/editor/ui/components/TimelineScale.h"
#include "playback/editor/ui/components/TrackTreeModel.h"
#include "playback/state/EditorAction.h"

#include "imgui.h"

#include <optional>
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

    void                      submitSeek(PanelContext const& ctx, int tick);
    [[nodiscard]] std::string resolveKeyframeTargetCamera(PanelContext const& ctx) const;

    void drawTitleRow(PanelContext const& ctx, Layout const& layout, int displayTick, bool allowInput);
    void drawTrackHeaders(PanelContext const& ctx, Layout const& layout, bool allowInput);
    void drawRuler(
        PanelContext const&  ctx,
        Layout const&        layout,
        TimelineScale const& scale,
        int                  displayTick
    ); // Drawn after the track rows so the row bands do not cover it.
    void drawTrackGrid(PanelContext const& ctx, Layout const& layout, TimelineScale const& scale);
    // In/out markers for the export range, drawn over the canvas and draggable by their handles.
    void drawExportRange(PanelContext const& ctx, Layout const& layout, TimelineScale const& scale, bool allowInput);

    // Camera-switch lane: each segment names the camera (or first person) that renders over its range.
    void drawSequenceLane(
        PanelContext const&  ctx,
        Layout const&        layout,
        TimelineScale const& scale,
        float                top,
        float                bottom,
        bool                 allowInput,
        bool&                clickConsumed
    );
    void drawSequenceSegmentMenu(PanelContext const& ctx);
    void drawContextMenus(PanelContext const& ctx);

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
    bool           mDraggingPlayhead{};
    // 0 none, 1 in marker, 2 out marker.
    int         mExportMarkerDrag{};
    std::string mDraggingKeyframeCameraId;
    float       mDraggingKeyframeStartMouseX{};
    int         mDraggingKeyframeStartTick{};
    int         mDraggingKeyframeTick{};
    bool        mDraggingKeyframeMoved{};
    // Range-bar drag: 0 none, 1 left handle, 2 right handle, 3 whole window.
    int   mRangeDragMode{};
    float mRangeDragOriginX{};
    float mRangeDragOriginScroll{};
    float mRangeDragOriginZoom{1.0f};
    // Sequence edge drag: the segment whose start (or end) follows the mouse until release.
    std::string mSequenceDragId;
    bool        mSequenceDragEnd{};
    int         mSequenceDragTick{-1};
    // Segment the bind menu was opened for; kept separately so the menu survives selection changes.
    int         mSequenceMenuTick{-1};
    // Right-click menus. The tick is captured when the menu opens so it does not follow the cursor.
    std::string                                           mKeyframeMenuCameraId;
    int                                                   mKeyframeMenuTick{-1};
    int                                                   mKeyframeMenuTickEdit{-1};
    std::optional<state::editing::model::CameraKeyframe>  mKeyframeClipboard;
    std::string                                           mLaneMenuCameraId;
    int                                                   mLaneMenuTick{-1};
    std::string                                           mTrackMenuCameraId;
    bool                                                  mTrackMenuSequence{};
    int                                                   mRulerMenuTick{-1};
    std::string mSequenceMenuSegmentId;
    ImVec2      mSequenceMenuMin{};
    ImVec2      mSequenceMenuMax{};
    bool        mOpenSubjectPicker{false};
};

} // namespace playback::editor::ui
