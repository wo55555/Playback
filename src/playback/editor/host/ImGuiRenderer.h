#pragma once

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

namespace playback::editor::host {

class ImGuiRenderer final : public visuals::ReplayThumbnailCaptureProvider {
public:
    ImGuiRenderer();
    ~ImGuiRenderer();

    void                             setContext(state::EditorContext* context);
    void                             requestReplayThumbnailCapture() override;
    [[nodiscard]] bool               saveReplayThumbnail(std::filesystem::path const& output) override;
    [[nodiscard]] visuals::FrameTap& frameTap();
    [[nodiscard]] bool               captureSubmittedD3D12Frame(
        ID3D12Device*       device,
        ID3D12CommandQueue* queue,
        ID3D12Resource*     source,
        uint32_t            sourceState
    );
    [[nodiscard]] void* acquireReplayThumbnailTexture(std::string_view key, std::string_view png);

    bool               render(IDXGISwapChain* swapChain, bool allowFrameCapture = true);
    void               pollFrameCapture();
    [[nodiscard]] bool ownsSwapChain(IDXGISwapChain* swapChain) const;
    bool               beforeResize(IDXGISwapChain* swapChain);
    void               afterPresent(IDXGISwapChain* swapChain, long result);
    bool               shutdown();

private:
    bool renderInternal(IDXGISwapChain* swapChain, bool allowUi, bool allowFrameCapture);

    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

extern ImGuiRenderer gImGuiRenderer;

} // namespace playback::editor::host
