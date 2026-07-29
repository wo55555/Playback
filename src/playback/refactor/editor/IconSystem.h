#pragma once

#include "imgui.h"

namespace playback::refactor::editor {

class IconSystem {
public:
    // Load font (call in Editor::initialize)
    void loadFonts();

    // Push icon font onto ImGui stack (call before drawing with icons)
    void pushIconFont();
    void popIconFont();

    // Glyph range for merged icon font
    static const ImWchar* getGlyphRange();

    // Singleton access
    static IconSystem& getInstance();
};

} // namespace playback::refactor::editor