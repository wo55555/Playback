#pragma once

#include "playback/functions/render/ReplayThumbnail.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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

    bool               render(IDXGISwapChain* swapChain, bool allowFrameCapture = true);
    // The Present hook calls this while the matching BGFX flip boundary is active.
    [[nodiscard]] bool captureOfflineFrame(
        IDXGISwapChain*             swapChain,
        std::optional<std::uint32_t> backBufferIndex = std::nullopt
    );
    void               pollFrameCapture();
    [[nodiscard]] bool ownsSwapChain(IDXGISwapChain* swapChain) const;
    bool               beforeResize(IDXGISwapChain* swapChain);
    void               afterPresent(IDXGISwapChain* swapChain, long result);
    bool               shutdown();

private:
    bool renderInternal(
        IDXGISwapChain*             swapChain,
        bool                        allowUi,
        bool                        allowFrameCapture,
        std::optional<std::uint32_t> backBufferIndex = std::nullopt
    );

    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

extern ImGuiRenderer gImGuiRenderer;

} // namespace playback::editor::renderer
