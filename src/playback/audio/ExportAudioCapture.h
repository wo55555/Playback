#pragma once

#include <cstdint>
#include <filesystem>

namespace playback::exporting {
struct ExportSettings;
}

// Export-audio capture: takes over the game's FMOD system during export, drives the mixer from the frame ledger, and
// captures the master-group mix to a sidecar WAV. Registers policy callbacks on playback::audio::hooks.
namespace playback::audio::exportcap {

// Wire the policy callbacks into the audio hooks. Call only after hooks::install() succeeded. Safe to call when FMOD
// symbols are missing (returns false and the whole audio path stays inert).
bool install();
void uninstall();

// Arm the session at export start (before the timeline seek). Gates the game mixer and switches the FMOD output
// to NOSOUND_NRT. endTick/frameRate come from the export plan.
void exportBegin(exporting::ExportSettings const& settings);

// Disarm and clean up. Idempotent; called on success, cancel, and failure alike.
void exportEnd();

// Advance the sample ledger once per accepted frame. Called from ExportCoordinator::trySubmit after the writer
// accepts a frame; this is the only audio-advance entry point.
void onFrameAccepted(uint64_t frameIndex);

// Close the sidecar WAV after the video is fully written, so the muxer can consume it.
void finalizeSidecar();

[[nodiscard]] bool                  sessionActive();
[[nodiscard]] bool                  capturing(); // DSP attached and receiving PCM
[[nodiscard]] std::filesystem::path sidecarPath();

} // namespace playback::audio::exportcap
