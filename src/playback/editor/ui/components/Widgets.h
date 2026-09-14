#pragma once

#include "imgui.h"

#include <string>

namespace playback::editor::ui::widgets {

constexpr int kTicksPerSecond = 20;

// mm:ss.cc, shared by every panel that shows a timeline position.
[[nodiscard]] std::string formatTick(int tick);

// Ruler labels: always m:ss so the unit is unambiguous ("0:05", "1:05").
[[nodiscard]] std::string formatTickCompact(int tick);

[[nodiscard]] float iconButtonSize();

bool iconButton(char const* id, char const* icon, char const* tooltip, bool enabled = true);

// Icon button that stays lit while `active`, for view toggles in toolbars.
bool iconToggle(char const* id, char const* icon, char const* tooltip, bool active, bool enabled = true);

// Icon glyphs are drawn larger than body text so they read as buttons rather than characters.
[[nodiscard]] float iconGlyphSize();

// Centres an icon glyph on its ink box inside a square of `boxSize`.
void drawIconCentred(ImDrawList* drawList, char const* icon, ImVec2 const& origin, float boxSize, ImU32 color);

// Y offset that puts the cap height of body text on a box's centre line.
[[nodiscard]] float textOffsetInBox(float boxHeight);

// Same, but centres the run's real glyph ink: CJK ideographs sit well below the Latin cap height.
[[nodiscard]] float textOffsetInBox(float boxHeight, char const* text);

// Draw Y putting the run's ink centre on `centreY`; prefer it when the box origin is fractional.
[[nodiscard]] float textYForCentre(float centreY, char const* text);

// Optical centre of a run drawn at `drawY`; align icons to this rather than to the row.
[[nodiscard]] float textInkCentre(float drawY, char const* text);

// Icon whose ink centre lands on `centre`, for rows whose height is not a whole number of pixels.
void drawIconAtCentre(ImDrawList* drawList, char const* icon, ImVec2 const& centre, ImU32 color);

// Transport and window glyphs drawn from primitives so they share one weight and exact centring.
enum class VectorIcon { SkipStart, StepBack, Play, Pause, StepForward, SkipEnd, Maximize, Restore };

void drawVectorIcon(ImDrawList* drawList, ImVec2 const& centre, float boxSize, ImU32 color, VectorIcon icon);

bool vectorIconButton(char const* id, VectorIcon icon, char const* tooltip, bool active = false, bool enabled = true);

// Tooltip for the last item, drawn on the foreground list so panel windows never cover it.
void itemTooltip(char const* text);

// Same, for hit regions tested manually rather than through an ImGui item.
void hoverTooltip(char const* text);

// Flat combo-looking chip, exactly one icon button tall; returns true when clicked so callers can open a popup.
bool dropdownChip(
    char const* id,
    char const* label,
    char const* tooltip = nullptr,
    bool        enabled = true,
    char const* icon    = nullptr
);

// Width the chip will occupy, for callers that centre a group of controls.
[[nodiscard]] float dropdownChipWidth(char const* label, char const* icon = nullptr);

// Draws the shared "no active project" placeholder.
void noActiveProjectPlaceholder();

} // namespace playback::editor::ui::widgets
