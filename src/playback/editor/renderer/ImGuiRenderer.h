#pragma once

#include <memory>
#include <filesystem>
#include <string_view>

struct IDXGISwapChain;

namespace playback::editor {
class EditorContext;

} // namespace playback::editor

namespace playback::editor::renderer {

class ImGuiRenderer {
public:
    ImGuiRenderer();
    ~ImGuiRenderer();

    void setContext(EditorContext* context);
    void requestReplayThumbnailCapture();
    [[nodiscard]] bool saveReplayThumbnail(std::filesystem::path const& output);
    [[nodiscard]] void* acquireReplayThumbnailTexture(std::string_view key, std::string_view png);
    void clearReplayThumbnailTextures();

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
