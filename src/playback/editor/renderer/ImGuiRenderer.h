#pragma once

#include <memory>

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
