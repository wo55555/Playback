#pragma once

#include "playback/editor/editing/models/CameraKeyframe.h"
#include "playback/editor/keyframe/CameraTimelineEvaluator.h"
#include "playback/editor/ui/components/Splitter.h"
#include "playback/editor/ui/menus/ViewportMenu.h"


#include "imgui.h"

#include <optional>
#include <string>

namespace playback::editor::ui {

class ViewportPanel {
public:
    void                      draw(bool maximized = false);
    void                      setGameTexture(ImTextureID texture);
    void                      setVideoAspectRatio(float aspectRatio);
    void                      resetCameraControl();
    [[nodiscard]] Rect        videoRect() const { return mVideoRect; }
    [[nodiscard]] ImTextureID gameTexture() const { return mGameTexture; }

private:
    void handleCameraControl(bool hovered, bool active);
    void drawCameraPathOverlay(ImDrawList& drawList);
    void drawTransportControls();

    ImTextureID          mGameTexture{};
    float                mVideoAspectRatio{16.0f / 9.0f};
    Rect                 mVideoRect{};
    ViewportMenu         mContextMenu;
    std::optional<keyframe::CameraRenderState> mViewportCamera;
    int                                        mViewportCameraTick{-1};
};

} // namespace playback::editor::ui
