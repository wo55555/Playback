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
#include <utility>
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
    bool               addMarkerAtPlayhead(PanelContext const& ctx);
    [[nodiscard]] bool canSplitClipAtPlayhead(PanelContext const& ctx) const;
    bool               splitClipAtPlayhead(PanelContext const& ctx);
    [[nodiscard]] bool canTrimClipAtPlayhead(PanelContext const& ctx, bool trimIn) const;
    bool               trimClipAtPlayhead(PanelContext const& ctx, bool trimIn);
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

    [[nodiscard]] TimelineTool tool() const { return mTool; }
    void                       setTool(TimelineTool tool) { mTool = tool; }

private:
    struct Layout {
        ImVec2 fullMin{};
        ImVec2 fullMax{};
        float  titleBottom{};
        float  sceneTabBottom{};
        float  rulerTop{};
        float  headerHeight{};
        float  bodyTop{};
        float  camerasTop{};
        float  bodyBottom{};
        float  toolStripRight{};
        float  listLeft{};
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
    int snapToEditPoints(
        PanelContext const&                                 ctx,
        int                                                 tick,
        float                                               pixelsPerTick,
        bool                                                includePlayhead,
        bool                                                includeExportRange,
        std::function<bool(std::string const&, int)> const& skipKey,
        std::string_view                                    skipMarkerId,
        std::string_view                                    skipClipId           = {},
        std::optional<int>                                  skipSpeedKeyTick     = std::nullopt,
        std::optional<int>                                  skipFreezeKeyTick    = std::nullopt,
        std::optional<int>                                  skipTimelapseKeyTick = std::nullopt
    );
    bool addMarkerAt(PanelContext const& ctx, int tick);
    bool addFreezeKeyframeAt(PanelContext const& ctx, int tick);
    bool addTimelapseKeyframeAt(PanelContext const& ctx, int tick);
    // Selections that point at objects an queued action has not created yet; applied once they exist.
    void                      clearPendingSelection();
    void queueKeyframeSelection(std::vector<state::editing::model::SelectedKeyframe> keys);
    void queueMarkerSelection(std::string markerId);
    void                      queueSpeedTrackSelection();
    void                      queueSpeedKeyframeSelection(int tick);
    void                      queueFreezeTrackSelection();
    void                      queueFreezeKeyframeSelection(int tick);
    void                      queueTimelapseTrackSelection();
    void                      queueTimelapseKeyframeSelection(int tick);
    void applyPendingSelection(PanelContext const& ctx);
    [[nodiscard]] std::string markerAt(
        PanelContext const&  ctx,
        Layout const&        layout,
        TimelineScale const& scale,
        ImVec2               mouse
    ) const;
    void drawMarkers(PanelContext const& ctx, Layout const& layout, TimelineScale const& scale);

    void drawTitleRow(PanelContext const& ctx, Layout const& layout, int displayTick);
    void drawSceneTabs(PanelContext const& ctx, Layout const& layout, bool allowInput);
    void drawToolStrip(PanelContext const& ctx, Layout const& layout, bool allowInput);
    void drawTrackHeaders(PanelContext const& ctx, Layout const& layout, bool allowInput);
    // Drawn after the track rows so the row bands do not cover it.
    void drawRuler(PanelContext const& ctx, Layout const& layout, TimelineScale const& scale, int displayTick);
    void drawTrackGrid(PanelContext const& ctx, Layout const& layout, TimelineScale const& scale);
    // In/out markers for the export range, drawn over the canvas and draggable by their handles.
    void drawExportRange(PanelContext const& ctx, Layout const& layout, TimelineScale const& scale, bool allowInput);

    void drawContextMenus(PanelContext const& ctx);

    void drawRangeBar(PanelContext const& ctx, Layout const& layout, bool allowInput);
    float drawTimecodeReadout(PanelContext const& ctx, float height, int displayTick);
    float drawTransportGroup(PanelContext const& ctx, float height, float width, float minStartX, float maxRightX);

    TrackTreeModel mTrackTree;
    Animator       mAnimator;
    TimelineTool   mTool{TimelineTool::Select};
    std::string    mLastActiveSceneId;
    float          mZoomScale{1.0f};
    float          mScrollX{};
    float          mScrollY{};
    float          mTrackListWidthRatio{0.18f};
    int            mPendingSeekTick{-1};
    int            mRulerDragTick{-1};
    std::string    mTrackSearch;
    bool           mSnapEnabled{true};
    bool           mDraggingPlayhead{};
    // 0 none, 1 in marker, 2 out marker.
    int         mExportMarkerDrag{};
    int         mExportDragInTick{-1};
    int         mExportDragOutTick{-1};
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
    // Right-click menus. The tick is captured when the menu opens so it does not follow the cursor.
    std::string                                           mKeyframeMenuCameraId;
    int                                                   mKeyframeMenuTick{-1};
    int                                                   mKeyframeMenuTickEdit{-1};
    bool                                                  mKeyframeMenuGroup{};
    std::vector<ClipboardKey>                             mKeyframeClipboard;
    std::string                                           mLaneMenuCameraId;
    int                                                   mLaneMenuTick{-1};
    std::string                                           mTrackMenuCameraId;
    // The camera whose track row is being dragged to a new priority; empty when no reorder is in progress.
    std::string mTrackDragCameraId;
    // Set after Escape cancels a track drag so holding the button down does not immediately start it again.
    bool                                                  mTrackDragCancelled{};
    int                                                   mRulerMenuTick{-1};
    // Inline rename field of the track menu; refilled whenever the menu targets another camera.
    std::string                                           mRenameCameraId;
    std::array<char, 128>                                 mRenameBuffer{};
    // Scene tabs, clip context menu, inline rename, and drag state.
    std::string           mSceneMenuId;
    std::string           mSceneRenameId;
    bool                  mFocusSceneRename{false};
    std::array<char, 128> mSceneRenameBuffer{};
    std::string           mClipMenuClipId;
    int                   mClipMenuTick{-1};
    std::string           mClipRenameId;
    bool                  mFocusClipRename{false};
    std::array<char, 128> mClipRenameBuffer{};
    // 0 none, 1 trim in, 2 trim out, 3 move.
    int         mClipDragMode{};
    std::string mClipDragCameraId;
    std::string mClipDragId;
    float       mClipDragStartMouseX{};
    int         mClipDragOrigInTick{};
    int         mClipDragOrigOutTick{};
    int         mClipDragInTick{};
    int         mClipDragOutTick{};
    bool        mClipDragMoved{};

    // Grabbing a key that is part of a multi-selection moves the whole set by one delta.
    bool mGroupDrag{};
    int  mGroupDragDelta{};
    // A press on empty canvas becomes a box select once it travels a few pixels; otherwise it is a click.
    bool        mBoxPending{};
    bool        mBoxSelecting{};
    bool        mBoxAdditive{};
    ImVec2      mBoxStart{};
    std::string mBoxClickCameraId;
    bool        mPenPendingAdd{};
    int         mPenPendingTick{-1};
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
    bool                                                 mPendingSpeedTrackSelection{false};
    std::optional<int>                                   mPendingSpeedKeyframeSelection;
    bool                                                 mPendingFreezeTrackSelection{false};
    std::optional<int>                                   mPendingFreezeKeyframeSelection;
    bool                                                 mPendingTimelapseTrackSelection{false};
    std::optional<int>                                   mPendingTimelapseKeyframeSelection;
    int                                                  mPendingSelectionFrames{};

    int   mDraggingSpeedKeyframeOriginTick{-1};
    float mDraggingSpeedKeyframeStartMouseX{};
    int   mDraggingSpeedKeyframeTick{-1};
    bool  mDraggingSpeedKeyframeMoved{};
    int   mSpeedKeyframeMenuTick{-1};
    int   mSpeedLaneMenuTick{-1};

    int   mDraggingFreezeKeyframeOriginTick{-1};
    float mDraggingFreezeKeyframeStartMouseX{};
    int   mDraggingFreezeKeyframeTick{-1};
    bool  mDraggingFreezeKeyframeMoved{};
    int   mFreezeKeyframeMenuTick{-1};
    int   mFreezeLaneMenuTick{-1};

    int   mDraggingTimelapseKeyframeOriginTick{-1};
    float mDraggingTimelapseKeyframeStartMouseX{};
    int   mDraggingTimelapseKeyframeTick{-1};
    bool  mDraggingTimelapseKeyframeMoved{};
    int   mTimelapseKeyframeMenuTick{-1};
    int   mTimelapseLaneMenuTick{-1};

    struct OutputDurationCache {
        bool                                  valid{};
        int                                   totalTicks{};
        bool                                  camerasEmpty{};
        state::editing::model::SpeedTrack     speedTrack;
        state::editing::model::TimelapseTrack timelapseTrack;
        std::vector<std::pair<int, int>>      slices;
        double                                outputSeconds{};
    } mOutputDurationCache;
};

} // namespace playback::editor::ui
