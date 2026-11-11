#pragma once

// Editor preview audio: an output gain on the FMOD master group, plus Flashback's rule that a paused replay starts no
// new sounds. The export capture taps the mix ahead of this gain, so none of it reaches an exported file.
namespace playback::audio::preview {

void                setMuted(bool muted);
[[nodiscard]] bool  isMuted();
void                setVolume(float volume);
[[nodiscard]] float volume();
void                setSilentWhilePaused(bool silent);
[[nodiscard]] bool  isSilentWhilePaused();

// Main thread, once per client update.
void update(bool replayActive, bool replayPaused, bool replaySeeking, bool exporting);

// Any thread: a world sound the game starts right now should be stopped at once.
[[nodiscard]] bool shouldDropNewSounds();

// Takes the gain off the mixer again and returns to the pass-through state.
void reset();

} // namespace playback::audio::preview
