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
    void resetAudioDrafts();

    // Each returns true when it owns the current selection and has drawn it.
    bool drawClip(PanelContext const& ctx);
    bool drawCamera(PanelContext const& ctx);
    bool drawKeyframes(PanelContext const& ctx);
    bool drawKeyframe(PanelContext const& ctx);
    bool drawMarker(PanelContext const& ctx);
    bool drawSpeedKeyframe(PanelContext const& ctx);
    bool drawSpeedTrack(PanelContext const& ctx);
    bool drawFreezeKeyframe(PanelContext const& ctx);
    bool drawFreezeTrack(PanelContext const& ctx);
    bool drawTimelapseKeyframe(PanelContext const& ctx);
    bool drawTimelapseTrack(PanelContext const& ctx);
    bool drawTimeOfDayKeyframes(PanelContext const& ctx);
    bool drawTimeOfDayKeyframe(PanelContext const& ctx);
    bool drawTimeOfDayTrack(PanelContext const& ctx);
    bool drawWeatherKeyframes(PanelContext const& ctx);
    bool drawWeatherKeyframe(PanelContext const& ctx);
    bool drawWeatherTrack(PanelContext const& ctx);
    bool drawAudioTrack(PanelContext const& ctx);
    bool drawAudioClip(PanelContext const& ctx);
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
    int                         mSpeedBezierDragTick{-1};
    int                         mSpeedBezierDragHandle{};
    state::editing::model::Vec2 mSpeedBezierDraftCtrl1{0.42f, 0.0f};
    state::editing::model::Vec2 mSpeedBezierDraftCtrl2{0.58f, 1.0f};
    int                         mSpeedKeyDragTick{-1};
    bool                        mSpeedKeyDragging{false};
    float                       mSpeedKeyDragValue{1.0f};
    int                         mFreezeDelayDragTick{-1};
    bool                        mFreezeDelayDragging{false};
    int                         mFreezeDelayDragValue{0};
    std::optional<int>          mTimelapseTextTick;
    std::array<char, 64>        mTimelapseTextBuffer{};
    bool                        mTimelapseTextEditing{false};
    int                         mTimeOfDayBezierDragTick{-1};
    int                         mTimeOfDayBezierDragHandle{};
    state::editing::model::Vec2 mTimeOfDayBezierDraftCtrl1{0.42f, 0.0f};
    state::editing::model::Vec2 mTimeOfDayBezierDraftCtrl2{0.58f, 1.0f};
    int                         mTimeOfDayKeyDragTick{-1};
    bool                        mTimeOfDayKeyDragging{false};
    int                         mTimeOfDayKeyDragValue{6000};
    bool                        mDaytimeOverrideDragging{false};
    int                         mDaytimeOverrideDragValue{6000};
    std::string                 mAudioTrackNameId;
    std::array<char, 128>       mAudioTrackNameBuffer{};
    bool                        mAudioTrackNameEditing{false};
    std::string                 mAudioTrackVolumeDragId;
    bool                        mAudioTrackVolumeDragging{false};
    float                       mAudioTrackVolumeDragValue{1.0f};
    std::string                 mAudioClipPathId;
    std::array<char, 512>       mAudioClipPathBuffer{};
    bool                        mAudioClipPathEditing{false};
    std::string                 mAudioClipVolumeDragId;
    bool                        mAudioClipVolumeDragging{false};
    float                       mAudioClipVolumeDragValue{1.0f};
};

} // namespace playback::editor::ui
