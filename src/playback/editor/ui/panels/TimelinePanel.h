#pragma once

#include "playback/editor/ui/PanelContext.h"
#include "playback/editor/ui/components/Animator.h"
#include "playback/editor/ui/components/TimelineScale.h"
#include "playback/editor/ui/components/TrackTreeModel.h"
#include "playback/state/EditorAction.h"

#include "imgui.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playback::editor::ui {

class TimelinePanel {
public:
    void draw(PanelContext const& ctx, bool allowInput);

    void seekTo(PanelContext const& ctx, int tick);
    void seekRelative(PanelContext const& ctx, int tickDelta);
    void seekAdjacentEditPoint(PanelContext const& ctx, bool forward);
    bool addKeyframeAtPlayhead(PanelContext const& ctx);
    bool deleteSelection(PanelContext const& ctx);
    bool selectAllKeyframes(PanelContext const& ctx);
    // Copies the selected keyframes; paste keeps their spacing and lands the earliest one on the playhead.
    bool copySelection(PanelContext const& ctx);
    bool pasteAtPlayhead(PanelContext const& ctx);
    bool addMarkerAtPlayhead(PanelContext const& ctx);
    void seekAdjacentMarker(PanelContext const& ctx, bool forward);
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

    struct ClipboardKey {
        std::string                           cameraId;
        int                                   offset{};
        state::editing::model::CameraKeyframe pose;
    };

    void                      submitSeek(PanelContext const& ctx, int tick);
    [[nodiscard]] std::string resolveKeyframeTargetCamera(PanelContext const& ctx) const;
    // Single-camera clips go to `targetCameraId` when given; multi-camera clips return to their own tracks.
    bool pasteKeyframes(PanelContext const& ctx, int anchorTick, std::string const& targetCameraId);
    // Shift-drag snapping onto keyframes, markers, the playhead, in/out and segment edges within a few pixels.
    // The thing being dragged is excluded so it does not stick to its own position.
    int  snapToEditPoints(
        PanelContext const&                                 ctx,
        int                                                 tick,
        float                                               pixelsPerTick,
        bool                                                includePlayhead,
        bool                                                includeExportRange,
        std::function<bool(std::string const&, int)> const& skipKey,
        std::string_view                                    skipMarkerId
    );
    bool addMarkerAt(PanelContext const& ctx, int tick);
    // Selections that point at objects an queued action has not created yet; applied once they exist.
    void queueKeyframeSelection(std::vector<state::editing::model::SelectedKeyframe> keys);
    void queueMarkerSelection(std::string markerId);
    void applyPendingSelection(PanelContext const& ctx);
    [[nodiscard]] std::string markerAt(
        PanelContext const&  ctx,
        Layout const&        layout,
        TimelineScale const& scale,
        ImVec2               mouse
    ) const;
    void drawMarkers(PanelContext const& ctx, Layout const& layout, TimelineScale const& scale);

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
    bool                                                  mKeyframeMenuGroup{};
    std::vector<ClipboardKey>                             mKeyframeClipboard;
    std::string                                           mLaneMenuCameraId;
    int                                                   mLaneMenuTick{-1};
    std::string                                           mTrackMenuCameraId;
    bool                                                  mTrackMenuSequence{};
    int                                                   mRulerMenuTick{-1};
    // Inline rename field of the track menu; refilled whenever the menu targets another camera.
    std::string                                           mRenameCameraId;
    std::array<char, 128>                                 mRenameBuffer{};
    std::string mSequenceMenuSegmentId;

    // Grabbing a key that is part of a multi-selection moves the whole set by one delta.
    bool mGroupDrag{};
    int  mGroupDragDelta{};
    // A press on empty canvas becomes a box select once it travels a few pixels; otherwise it is a click.
    bool        mBoxPending{};
    bool        mBoxSelecting{};
    bool        mBoxAdditive{};
    ImVec2      mBoxStart{};
    std::string mBoxClickCameraId;
    // Marker pins live in the ruler band: drag to move, right-click for the menu.
    std::string           mMarkerDragId;
    float                 mMarkerDragStartX{};
    int                   mMarkerDragTick{-1};
    bool                  mMarkerDragMoved{};
    std::string           mMarkerMenuId;
    std::string           mMarkerRenameId;
    std::array<char, 128> mMarkerRenameBuffer{};
    int                   mSnapGuideTick{-1};

    std::vector<state::editing::model::SelectedKeyframe> mPendingKeySelection;
    std::string                                          mPendingMarkerSelection;
    int                                                  mPendingSelectionFrames{};
};

} // namespace playback::editor::ui
