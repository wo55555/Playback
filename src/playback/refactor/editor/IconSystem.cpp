#include "IconSystem.h"

#include "playback/refactor/editor/iconfont.h"

#include "imgui.h"

namespace playback::refactor::editor {

IconSystem& IconSystem::getInstance() {
    static IconSystem instance;
    return instance;
}

void IconSystem::loadFonts() {
    ImGuiIO& io = ImGui::GetIO();

    // 1) Main font (loaded first)
    io.Fonts->AddFontFromFileTTF("resources/fonts/Inter-Regular.ttf", 16.0f);

    // 2) Icon font (merged, PUA range)
    ImFontConfig cfg;
    cfg.MergeMode  = true;
    cfg.PixelSnapH = true;
    cfg.GlyphOffset.y = 1.0f;
    io.Fonts->AddFontFromFileTTF(Playback::Editor::Icons::kFontPath, 16.0f, &cfg, getGlyphRange());

    // 3) Build
    io.Fonts->Build();
}

void IconSystem::pushIconFont() {
    // Icon font is merged into the main font, so no separate push needed
    // This is a no-op if fonts are merged
}

void IconSystem::popIconFont() {
    // No-op when fonts are merged
}

const ImWchar* IconSystem::getGlyphRange() {
    static const ImWchar range[] = {
        0xe000, // Lucide font start
        0xe6ff, // Lucide font end (actual end at 0xe6fd)
        0       // terminator
    };
    return range;
}

} // namespace playback::refactor::editor