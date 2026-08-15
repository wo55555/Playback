#pragma once

#include "playback/editor/ui/components/Splitter.h"
#include "playback/editor/ui/menus/ViewportMenu.h"


#include "imgui.h"

#include <string>

namespace playback::editor::ui {

class ViewportPanel {
public:
    void                      draw(bool maximized = false);
    void                      setGameTexture(ImTextureID texture);
    void                      setVideoAspectRatio(float aspectRatio);
    [[nodiscard]] Rect        videoRect() const { return mVideoRect; }
    [[nodiscard]] ImTextureID gameTexture() const { return mGameTexture; }

private:
    void drawTransportControls();

    ImTextureID          mGameTexture{};
    float                mVideoAspectRatio{16.0f / 9.0f};
    Rect                 mVideoRect{};
    ViewportMenu         mContextMenu;
};

} // namespace playback::editor::ui
