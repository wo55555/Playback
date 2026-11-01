#pragma once

namespace playback::visuals {

// The replay observer is a spectator, so the HUD hides the hotbar, hunger, armor and experience. While the view is
// driven by the recorded player, these hooks make the HUD take the recorded player's game mode instead. The inventory
// and attributes it then draws are mirrored onto the observer by the replay session.
// Installs the hooks on first use.
void installReplayHudHooks();

// Removes the hooks; called when the mod unloads.
void releaseReplayHudHooks();

// The recorded player's game mode (GameType value), or -1 while unknown. Safe to call from any thread.
void setReplayHudGameType(int gameType);

} // namespace playback::visuals
