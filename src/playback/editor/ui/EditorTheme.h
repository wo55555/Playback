#pragma once

#include "imgui.h"

#include <cstdint>

namespace playback::editor::ui {

struct EditorTheme {
    // Color palette
    ImU32 bgPanel  = IM_COL32(0x1a, 0x1a, 0x1a, 0xff);
    ImU32 bgHeader = IM_COL32(0x25, 0x25, 0x25, 0xff);
    ImU32 border   = IM_COL32(0x3a, 0x3a, 0x3a, 0xff);
    ImU32 text     = IM_COL32(0xe0, 0xe0, 0xe0, 0xff);
    ImU32 textDim  = IM_COL32(0x90, 0x90, 0x90, 0xff);
    ImU32 accent   = IM_COL32(0x3a, 0x8c, 0xf0, 0xff);
    ImU32 selected = IM_COL32(0xf0, 0xc0, 0x20, 0xff);
    ImU32 hover    = IM_COL32(0x3a, 0x5a, 0x8c, 0x99);
    ImU32 success  = IM_COL32(0x3a, 0xf0, 0x3a, 0xff);
    ImU32 warning  = IM_COL32(0xf0, 0xc0, 0x20, 0xff);

    // Spacing
    float panelPadding  = 8.0f;
    float itemSpacing   = 4.0f;
    float frameRounding = 4.0f;
    float trackRounding = 0.0f;

    // Font sizes
    float fontDefault = 14.0f;
    float fontTitle   = 16.0f;
    float fontSmall   = 14.0f;

    void apply() const;
};

} // namespace playback::editor::ui
