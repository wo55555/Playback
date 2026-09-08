#pragma once

#include "playback/editor/ui/PanelContext.h"
#include "playback/editor/ui/components/Splitter.h"

#include "imgui.h"

#include <string>

namespace playback::editor::ui {

class ViewportPanel {
public:
    void                      draw(PanelContext const& ctx, bool maximized = false);
    void                      setGameTexture(ImTextureID texture);
    void                      setVideoAspectRatio(float aspectRatio);
    [[nodiscard]] Rect        videoRect() const { return mVideoRect; }
    [[nodiscard]] ImTextureID gameTexture() const { return mGameTexture; }

private:
    void drawTransportControls(PanelContext const& ctx);

    ImTextureID mGameTexture{};
    float       mVideoAspectRatio{16.0f / 9.0f};
    Rect        mVideoRect{};
};

} // namespace playback::editor::ui
