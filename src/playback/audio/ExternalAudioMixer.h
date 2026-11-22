#pragma once

#include "playback/audio/ExternalAudioCache.h"
#include "playback/exporting/ExportTypes.h"
#include "playback/state/editing/models/Track.h"

#include <filesystem>
#include <vector>

namespace playback::audio::external {

[[nodiscard]] double effectiveAudioClipDurationSeconds(
    state::editing::model::AudioClip const& clip,
    DecodedAudioAsset const*                asset
) noexcept;

[[nodiscard]] bool hasPlayableExternalAudio(std::vector<state::editing::model::AudioTrack> const& tracks) noexcept;

[[nodiscard]] bool mixExternalAudioForExport(
    exporting::CompiledExportPlan const&                  plan,
    std::vector<state::editing::model::AudioTrack> const& tracks,
    std::filesystem::path const&                          gameAudioSidecarWav,
    std::filesystem::path const&                          outputWavPath
);

} // namespace playback::audio::external
