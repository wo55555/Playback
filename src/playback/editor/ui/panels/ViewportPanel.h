#pragma once

#include "playback/editor/editing/models/CameraKeyframe.h"
#include "playback/editor/ui/components/Splitter.h"
#include "playback/editor/ui/menus/ViewportMenu.h"


#include "imgui.h"

#include <string>

namespace playback::editor::ui {

class ViewportPanel {
public:
    void               draw(bool maximized = false);
    void               setGameTexture(ImTextureID texture);
    void               setVideoAspectRatio(float aspectRatio);
    [[nodiscard]] Rect videoRect() const { return mVideoRect; }

private:
    void handleCameraControl(bool hovered, bool active);
    void handleGizmoDrag();
    void drawGizmo();
    void drawTransportControls();

    float                mFov{90.0f};
    ImTextureID          mGameTexture{};
    float                mVideoAspectRatio{16.0f / 9.0f};
    Rect                 mVideoRect{};
    ViewportMenu         mContextMenu;
    editing::model::Vec2 mViewportRotation{0, 0};
    editing::model::Vec3 mViewportAnchor{0, 80, 0};
};

} // namespace playback::editor::ui
