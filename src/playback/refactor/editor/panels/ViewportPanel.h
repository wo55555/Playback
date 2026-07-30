#pragma once

#include "playback/refactor/editor/Splitter.h"
#include "playback/refactor/editor/contextmenu/ViewportMenu.h"
#include "playback/refactor/editor/models/CameraKeyframe.h"


#include "imgui.h"

#include <string>

namespace playback::refactor::editor {

class ViewportPanel {
public:
    void draw();
    void setGameTexture(ImTextureID texture);
    void setVideoAspectRatio(float aspectRatio);
    [[nodiscard]] Rect videoRect() const { return mVideoRect; }

private:
    void handleCameraControl(bool hovered, bool active);
    void handleGizmoDrag();
    void drawGizmo();

    float mFov{90.0f};
    ImTextureID mGameTexture{};
    float mVideoAspectRatio{16.0f / 9.0f};
    Rect mVideoRect{};
    ViewportMenu mContextMenu;
    Vec2  mViewportRotation{0, 0};
    Vec3  mViewportAnchor{0, 80, 0};
};

} // namespace playback::refactor::editor
