#pragma once

#include <cstddef>
#include <cstdint>

namespace playback::visuals {

// HUD parts the replay editor can hide.
enum class HudElement : uint8_t { Chat, BossBar, Title, Scoreboard, ActionBar, Hotbar };

inline constexpr size_t HudElementCount = 6;

// Registers the HUD bindings the UI pack reads. Installed with the mod, because the bindings must exist before the
// first HUD screen is built.
void hookReplayHudVisibility(bool enable);

// The user's choice. It only takes effect inside a replay; a normal world always draws its whole HUD.
void               setHudElementShown(HudElement element, bool shown);
[[nodiscard]] bool isHudElementShown(HudElement element);

// Export-scoped: hides the whole HUD so only the world reaches the captured frames.
void setHudHiddenForExport(bool hidden);

// What the HUD should leave out right now.
[[nodiscard]] bool isHudElementHiddenNow(HudElement element);

} // namespace playback::visuals
