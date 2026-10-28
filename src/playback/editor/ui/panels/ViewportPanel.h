#pragma once

#include "playback/editor/ui/PanelContext.h"
#include "playback/editor/ui/components/CameraPathOverlay.h"
#include "playback/editor/ui/components/Splitter.h"
#include "playback/replay/ReplaySession.h"

#include "imgui.h"

#include <optional>
#include <string>

namespace playback::editor::ui {

class ViewportPanel {
public:
    void               draw(PanelContext const& ctx, bool maximized = false);
    void               setGameTexture(ImTextureID texture);
    void               setVideoAspectRatio(float aspectRatio);
    void               clearCameraPath() { mCameraPath.clear(); }
    [[nodiscard]] Rect videoRect() const { return mVideoRect; }
    // Floating transport capsule over the video; empty when the viewport is not maximized.
    [[nodiscard]] Rect        overlayRect() const { return mOverlayRect; }
    [[nodiscard]] ImTextureID gameTexture() const { return mGameTexture; }

    [[nodiscard]] bool isInfoOverlayVisible() const { return mInfoOverlayVisible; }
    void               setInfoOverlayVisible(bool visible) { mInfoOverlayVisible = visible; }
    [[nodiscard]] bool isAutoPreviewEnabled() const { return mAutoPreview; }
    void               setAutoPreviewEnabled(bool enabled) { mAutoPreview = enabled; }

private:
    void drawToolbar(PanelContext const& ctx);
    void drawInfoOverlay(PanelContext const& ctx, ImVec2 const& videoMin, ImDrawList* drawList) const;
    void drawFloatingTransport(PanelContext const& ctx, ImVec2 const& sceneMin, ImVec2 const& sceneMax);
    // Nearest entity whose projected box covers the mouse in the last rendered view.
    [[nodiscard]] std::optional<replay::ReplaySession::ReplayEntityInfo>
         pickEntityAt(PanelContext const& ctx, ImVec2 const& mouse) const;
    void drawEntityPickMenu(PanelContext const& ctx);

    ImTextureID       mGameTexture{};
    float             mVideoAspectRatio{16.0f / 9.0f};
    Rect              mVideoRect{};
    Rect              mOverlayRect{};
    CameraPathOverlay mCameraPath;
    bool              mInfoOverlayVisible{true};
    bool              mAutoPreview{true};
    // Entity under the cursor when the viewport was right-clicked; the menu acts on it.
    std::optional<replay::ReplaySession::ReplayEntityInfo> mPickedEntity;
};

} // namespace playback::editor::ui
