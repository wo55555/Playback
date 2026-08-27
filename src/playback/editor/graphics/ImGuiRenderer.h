#pragma once

#include "playback/exporting/SaveableFramebufferQueue.h"
#include "playback/visuals/ReplayThumbnail.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

struct IDXGISwapChain;
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace playback::state {
class EditorContext;

} // namespace playback::state

namespace playback::editor::graphics {

class ImGuiRenderer final : public visuals::ReplayThumbnailCaptureProvider {
public:
    ImGuiRenderer();
    ~ImGuiRenderer();

    void               setContext(state::EditorContext* context);
    void               requestReplayThumbnailCapture() override;
    [[nodiscard]] bool saveReplayThumbnail(std::filesystem::path const& output) override;
    [[nodiscard]] exporting::SaveableFramebufferQueue& saveableFramebufferQueue();
    [[nodiscard]] void* acquireReplayThumbnailTexture(std::string_view key, std::string_view png);

    bool               render(IDXGISwapChain* swapChain, bool allowFrameCapture = true);
    bool               renderExportOverlay(IDXGISwapChain* swapChain);
    void               pollFrameCapture();
    [[nodiscard]] bool isD3D12RendererActive() const;
    [[nodiscard]] bool ownsSwapChain(IDXGISwapChain* swapChain) const;
    bool               beforeResize(IDXGISwapChain* swapChain);
    void               afterPresent(IDXGISwapChain* swapChain, long result);
    bool               shutdown();

private:
    bool renderInternal(IDXGISwapChain* swapChain, bool allowUi, bool allowFrameCapture, bool forceExportOverlay);

    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

extern ImGuiRenderer gImGuiRenderer;

} // namespace playback::editor::graphics
