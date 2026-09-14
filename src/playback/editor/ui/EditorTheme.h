#pragma once

#include "imgui.h"

#include <algorithm>

namespace playback::editor::ui::theme {

// Single source of truth for editor colours; panels reference these instead of local literals.
[[nodiscard]] constexpr ImU32 withAlpha(ImU32 colour, int alpha) {
    return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
}

constexpr ImU32 kBgPanel  = IM_COL32(0x19, 0x19, 0x19, 0xff);
constexpr ImU32 kBgHeader = IM_COL32(0x22, 0x22, 0x22, 0xff);
constexpr ImU32 kBorder   = IM_COL32(0x2c, 0x2c, 0x2c, 0xff);
constexpr ImU32 kText     = IM_COL32(0xd4, 0xd4, 0xd4, 0xff);
constexpr ImU32 kTextDim  = IM_COL32(0x8a, 0x8a, 0x8a, 0xff);
constexpr ImU32 kAccent   = IM_COL32(0x3a, 0x8c, 0xf0, 0xff);
constexpr ImU32 kSelected = IM_COL32(0xff, 0xd2, 0x3c, 0xff);
constexpr ImU32 kHover    = IM_COL32(0x3a, 0x5a, 0x8c, 0x99);
constexpr ImU32 kSuccess  = IM_COL32(0x66, 0xc8, 0x7a, 0xff);
constexpr ImU32 kWarning  = IM_COL32(0xf2, 0xa3, 0x3c, 0xff);
constexpr ImU32 kError    = IM_COL32(0xf0, 0x60, 0x5c, 0xff);

// Generic controls.
constexpr ImU32 kInputBg       = IM_COL32(0x12, 0x12, 0x12, 0xff);
constexpr ImU32 kInputBgHover  = IM_COL32(0x1e, 0x1e, 0x1e, 0xff);
constexpr ImU32 kInputBgActive = IM_COL32(0x24, 0x24, 0x24, 0xff);
constexpr ImU32 kButton        = IM_COL32(0x2a, 0x2a, 0x2a, 0xff);
constexpr ImU32 kButtonHover   = IM_COL32(0x38, 0x38, 0x38, 0xff);
constexpr ImU32 kButtonActive  = IM_COL32(0x44, 0x44, 0x44, 0xff);
constexpr ImU32 kScrollThumb   = IM_COL32(0x50, 0x50, 0x50, 0xff);
// Range bar zoom grips; the ring doubles as the rail outline that contains the accent fill.
constexpr ImU32 kGripBody = IM_COL32(0xc8, 0xcc, 0xd4, 0xff);
constexpr ImU32 kGripRing = IM_COL32(0x11, 0x11, 0x11, 0xff);

// Timeline surfaces.
constexpr ImU32 kTimelineBg        = IM_COL32(0x14, 0x14, 0x14, 0xff);
constexpr ImU32 kTimelineSidebar   = IM_COL32(0x1c, 0x1c, 0x1c, 0xff);
constexpr ImU32 kTimelineRuler     = IM_COL32(0x1a, 0x1a, 0x1a, 0xff);
constexpr ImU32 kTimelineTrackBg   = IM_COL32(0x18, 0x18, 0x18, 0xff);
constexpr ImU32 kLine              = IM_COL32(0x2e, 0x2e, 0x2e, 0xff);
constexpr ImU32 kRulerMajor        = IM_COL32(0x8a, 0x8a, 0x8a, 0xff);
constexpr ImU32 kRulerMinor        = IM_COL32(0x44, 0x44, 0x44, 0xff);
constexpr ImU32 kTrackGrid         = IM_COL32(0xff, 0xff, 0xff, 0x0c);
constexpr ImU32 kRowHighlight      = IM_COL32(0x2a, 0x2a, 0x2a, 0xff);
constexpr ImU32 kRowSelectedBorder = IM_COL32(0x5a, 0x6e, 0x86, 0xff);
constexpr ImU32 kKeyframe          = IM_COL32(0x46, 0xc8, 0xa0, 0xff);
constexpr ImU32 kKeyframeSelected  = IM_COL32(0xff, 0xd2, 0x3c, 0xff);
constexpr ImU32 kKeyframeDisabled  = IM_COL32(0x6a, 0x6a, 0x6a, 0xff);
constexpr ImU32 kKeyframeOutline   = IM_COL32(0x10, 0x10, 0x10, 0xff);
constexpr ImU32 kPlayhead          = IM_COL32(0xf2, 0xa3, 0x3c, 0xff);
constexpr ImU32 kPlayheadLabel     = IM_COL32(0x11, 0x11, 0x11, 0xff);
constexpr ImU32 kExportRange       = withAlpha(kAccent, 0x59);
// In/out markers, drawn in the ruler band the way Flashback does it.
constexpr ImU32 kExportBracket = IM_COL32(0xf0, 0xf2, 0xf5, 0xff);
constexpr ImU32 kExportBand    = IM_COL32(0xff, 0xaa, 0x00, 0x60);
constexpr ImU32 kTrackDisabled = IM_COL32(0x3a, 0x3a, 0x3a, 0xff);

// Cameras take a colour from this palette by creation order; the row fill uses it at low alpha.
constexpr ImU32 kTrackPalette[] = {
    IM_COL32(0x4f, 0x8f, 0xe0, 0xff),
    IM_COL32(0x46, 0xc8, 0xa0, 0xff),
    IM_COL32(0xe0, 0xa0, 0x40, 0xff),
    IM_COL32(0xd0, 0x6a, 0x6a, 0xff),
    IM_COL32(0xa0, 0x7f, 0xd8, 0xff),
    IM_COL32(0x58, 0xb4, 0xc8, 0xff),
    IM_COL32(0xc8, 0xc8, 0x58, 0xff),
    IM_COL32(0xd8, 0x88, 0xb8, 0xff),
};
constexpr int kTrackPaletteSize = static_cast<int>(sizeof(kTrackPalette) / sizeof(kTrackPalette[0]));

[[nodiscard]] constexpr ImU32 trackColor(int index) {
    int const wrapped = ((index % kTrackPaletteSize) + kTrackPaletteSize) % kTrackPaletteSize;
    return kTrackPalette[wrapped];
}
constexpr int kTrackFillAlpha         = 0x2e;
constexpr int kTrackFillSelectedAlpha = 0x52;

// Inspector surfaces.
constexpr ImU32 kInspectorHeader = IM_COL32(0x1e, 0x1e, 0x1e, 0xff);
constexpr ImU32 kSection         = IM_COL32(0x24, 0x24, 0x24, 0xff);
constexpr ImU32 kSectionHover    = IM_COL32(0x2e, 0x2e, 0x2e, 0xff);
constexpr ImU32 kFineDivider     = IM_COL32(0x33, 0x33, 0x33, 0xd2);
constexpr ImU32 kAxisX           = IM_COL32(0xe0, 0x5a, 0x5a, 0xff);
constexpr ImU32 kAxisY           = IM_COL32(0x6a, 0xc8, 0x6e, 0xff);
constexpr ImU32 kAxisZ           = IM_COL32(0x5a, 0x96, 0xf0, 0xff);
constexpr ImU32 kAxisNeutral     = IM_COL32(0x4a, 0x4a, 0x4a, 0xff);
constexpr ImU32 kRailBg          = IM_COL32(0x16, 0x16, 0x16, 0xff);
constexpr ImU32 kRailActive      = kAccent;

// Viewport and icon surfaces.
constexpr ImU32 kViewportBg    = IM_COL32(0x0d, 0x0d, 0x0d, 0xff);
constexpr ImU32 kIconActive    = IM_COL32(0xe6, 0xe8, 0xee, 0xff);
constexpr ImU32 kIconInactive  = IM_COL32(0xaa, 0xaa, 0xaa, 0xff);
constexpr ImU32 kIconHighlight = IM_COL32(0xff, 0xff, 0xff, 0xff);
constexpr ImU32 kOverlayBg     = IM_COL32(0x14, 0x14, 0x18, 0xd2);
constexpr ImU32 kOverlayHover  = IM_COL32(0x3a, 0x5a, 0x8c, 0xeb);
// Tooltips sit over panel greys, so they need a lighter fill, a distinct border and a drop shadow.
constexpr ImU32 kTooltipBg     = IM_COL32(0x2e, 0x2e, 0x32, 0xfa);
constexpr ImU32 kTooltipBorder = IM_COL32(0x4a, 0x4a, 0x50, 0xff);
constexpr ImU32 kTooltipShadow = IM_COL32(0x00, 0x00, 0x00, 0x80);
// Dims the editor behind modal dialogs so the dialog reads as the only live surface.
constexpr ImU32 kModalDim           = IM_COL32(0x00, 0x00, 0x00, 0x99);
constexpr ImU32 kAccentHover        = IM_COL32(0x5a, 0xa0, 0xf4, 0xff);
constexpr ImU32 kCameraPath         = IM_COL32(0xff, 0xff, 0x20, 0xeb);
constexpr ImU32 kCameraPathSelected = IM_COL32(0xff, 0x30, 0x30, 0xff);
constexpr ImU32 kCameraPathPlayhead = IM_COL32(0x20, 0xff, 0x20, 0xff);

constexpr float kPanelPadding     = 6.0f;
constexpr float kItemSpacing      = 3.0f;
constexpr float kFrameRounding    = 2.0f;
constexpr float kFineDividerWidth = 0.75f;

void apply();

} // namespace playback::editor::ui::theme

// Every editor dimension derives from the font size, so raising theme::kEditorFontScale rescales the whole editor.
namespace playback::editor::ui::metrics {

[[nodiscard]] inline float font() { return ImGui::GetFontSize(); }

// Ratio of the live font size to the rasterised base, i.e. the active UI scale.
[[nodiscard]] inline float scale() { return std::max(1.0f, font() / 14.0f); }

[[nodiscard]] inline float iconButton() { return font() + 16.0f * scale(); }
// Above body-text size so icons read as controls; Lucide fills its em box, so the margin stays small.
[[nodiscard]] inline float iconGlyph() { return font() * 1.22f; }
[[nodiscard]] inline float toolbarRow() { return iconButton() + 6.0f * scale(); }
[[nodiscard]] inline float ruler() { return font() + 8.0f * scale(); }
[[nodiscard]] inline float statusBar() { return font() + 6.0f * scale(); }
[[nodiscard]] inline float cameraRow() { return font() * 1.85f; }
[[nodiscard]] inline float subRow() { return font() * 1.55f; }
[[nodiscard]] inline float propertyRow() { return font() + 8.0f * scale(); }
[[nodiscard]] inline float rail() { return font() * 2.3f; }
// Tall enough for the round zoom grips that live on it.
[[nodiscard]] inline float rangeBar() { return font() * 1.3f; }
[[nodiscard]] inline float labelColumn() { return font() * 4.6f; }
[[nodiscard]] inline float splitter() { return 4.0f * scale(); }
[[nodiscard]] inline float trackSwatch() { return std::max(3.0f, 3.0f * scale()); }
[[nodiscard]] inline float keyframeRadius() { return std::max(4.0f, font() * 0.34f); }
[[nodiscard]] inline float gutter() { return 6.0f * scale(); }

} // namespace playback::editor::ui::metrics
