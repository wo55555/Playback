#pragma once

#include "playback/editor/ui/PanelContext.h"

#include <array>
#include <optional>
#include <string>

namespace playback::editor::ui {

enum class InspectorPage { Selection, Export, Preview, Settings };

class DetailsPanel {
public:
    void draw(PanelContext const& ctx);

    [[nodiscard]] InspectorPage page() const { return mPage; }
    void                        setPage(InspectorPage page) { mPage = page; }

private:
    void drawRail(float width, float height);
    void drawSelectionPage(PanelContext const& ctx);
    void drawExportPage(PanelContext const& ctx);
    void drawPreviewPage(PanelContext const& ctx);
    void drawSettingsPage(PanelContext const& ctx);
    void drawOptiPistonSection();

    // Each returns true when it owns the current selection and has drawn it.
    bool drawClip(PanelContext const& ctx);
    bool drawCamera(PanelContext const& ctx);
    bool drawKeyframes(PanelContext const& ctx);
    bool drawKeyframe(PanelContext const& ctx);
    bool drawMarker(PanelContext const& ctx);
    void drawOverview(PanelContext const& ctx);
    void drawKeyframeInterpolationRows(
        PanelContext const&                          ctx,
        state::editing::model::CameraEntity const&   camera,
        int                                          originalTick,
        state::editing::model::CameraKeyframe const& keyframe
    );

    InspectorPage        mPage{InspectorPage::Selection};
    std::optional<float> mPistonDurationDraft;
    // Local label buffer so typing is not overwritten by project state each frame.
    std::string                 mMarkerLabelId;
    std::array<char, 128>       mMarkerLabelBuffer{};
    bool                        mMarkerLabelEditing{};
    std::string                 mBezierDragCameraId;
    int                         mBezierDragTick{-1};
    int                         mBezierDragHandle{};
    state::editing::model::Vec2 mBezierDraftCtrl1{0.42f, 0.0f};
    state::editing::model::Vec2 mBezierDraftCtrl2{0.58f, 1.0f};
    std::string                 mSpeedDragClipId;
    bool                        mSpeedDragging{false};
    float                       mSpeedDragValue{1.0f};
};

} // namespace playback::editor::ui
