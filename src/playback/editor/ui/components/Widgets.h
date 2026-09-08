#pragma once

#include <string>

namespace playback::editor::ui::widgets {

constexpr int kTicksPerSecond = 20;

// mm:ss.cc, shared by every panel that shows a timeline position.
[[nodiscard]] std::string formatTick(int tick);

[[nodiscard]] float iconButtonSize();

bool iconButton(char const* id, char const* icon, char const* tooltip, bool enabled = true);

// Draws the shared "no active project" placeholder.
void noActiveProjectPlaceholder();

} // namespace playback::editor::ui::widgets
