#pragma once

#include "playback/functions/render/ReplayThumbnail.h"

#include <filesystem>
#include <memory>
#include <string_view>

struct IDXGISwapChain;

namespace playback::editor {
class EditorContext;

} // namespace playback::editor

namespace playback::editor::renderer {

class ImGuiRenderer final : public functions::render::ReplayThumbnailCaptureProvider {
public:
    ImGuiRenderer();
    ~ImGuiRenderer();

    void                                       setContext(EditorContext* context);
    void                                       requestReplayThumbnailCapture() override;
    [[nodiscard]] bool                         saveReplayThumbnail(std::filesystem::path const& output) override;
    [[nodiscard]] functions::render::FrameTap& frameTap();
    [[nodiscard]] void* acquireReplayThumbnailTexture(std::string_view key, std::string_view png);

    bool render(IDXGISwapChain* swapChain);
    bool beforeResize(IDXGISwapChain* swapChain);
    void afterPresent(IDXGISwapChain* swapChain, long result);
    bool shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

extern ImGuiRenderer gImGuiRenderer;

} // namespace playback::editor::renderer
