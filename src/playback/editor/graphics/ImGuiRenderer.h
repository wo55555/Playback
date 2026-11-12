#pragma once

#include "playback/visuals/FrameTap.h"
#include "playback/visuals/ReplayThumbnail.h"

#include <chrono>
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

    // Export capture of the back buffer, which holds the overlay-free world, so no MSAA or resize is needed.
    [[nodiscard]] bool                                  openExportCapture(uint32_t capacity, uint32_t downsample);
    void                                                closeExportCapture();
    [[nodiscard]] bool                                  armExportCapture(visuals::FrameTicket const& ticket);
    // Blocks for up to `wait` when no frame is ready yet.
    [[nodiscard]] std::optional<visuals::CapturedFrame> collectExportFrame(std::chrono::milliseconds wait = {});
    [[nodiscard]] visuals::FrameTapStatus               exportCaptureStatus() const;
    [[nodiscard]] void* acquireReplayThumbnailTexture(std::filesystem::path const& path);

    bool               render(IDXGISwapChain* swapChain, bool allowFrameCapture = true);
    bool               renderExportOverlay(IDXGISwapChain* swapChain);
    // Render thread, once the renderer has submitted a scene: takes an armed export capture from the back buffer
    // without waiting for the Present that follows. False leaves the capture to that Present.
    bool               captureSubmittedScene();
    // True while captureSubmittedScene has a backend to capture with.
    [[nodiscard]] bool canCaptureSubmittedScene() const;
    void               pollFrameCapture();
    [[nodiscard]] bool isD3D12RendererActive() const;
    [[nodiscard]] bool ownsSwapChain(IDXGISwapChain* swapChain) const;
    [[nodiscard]] bool beforeRendererReset(bool shuttingDown = false);
    void               afterRendererReset();
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
