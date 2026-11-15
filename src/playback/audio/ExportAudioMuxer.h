#pragma once

#include "playback/exporting/ExportTypes.h"

#include <filesystem>

// Second-pass ffmpeg muxer: combines the finished video with the captured sidecar WAV. No FMOD dependency.
namespace playback::audio::exportmux {

// Mux `videoPath` (silent MP4) with `sidecarWav` into a final MP4 with an audio track, replacing videoPath on success.
// Returns true when the audio was muxed in. On failure the silent video and the sidecar are kept.
bool muxIntoMp4(
    std::filesystem::path const& videoPath,
    std::filesystem::path const& sidecarWav,
    exporting::AudioCodec        codec,
    uint32_t                     bitrateKbps
);

} // namespace playback::audio::exportmux
