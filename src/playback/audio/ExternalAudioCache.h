#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace playback::audio::external {

enum class AudioLoadState : uint8_t { Pending, Ready, Failed };

struct DecodedAudioAsset {
    static constexpr uint32_t SampleRate     = 48000;
    static constexpr uint32_t Channels       = 2;
    static constexpr uint32_t PeaksPerSecond = 100; // 10ms per peak bucket (5 peaks per 20Hz tick)

    AudioLoadState       state{AudioLoadState::Pending};
    std::string          filePath;
    std::string          errorMessage;
    double               durationSeconds{0.0};
    std::vector<float>   pcmInterleaved; // Stereo interleaved 48kHz float32, range [-1, 1]
    std::vector<uint8_t> peakEnvelope;   // [0..255] peak amplitude per 10ms bucket

    [[nodiscard]] uint64_t frameCount() const noexcept { return pcmInterleaved.size() / Channels; }
    [[nodiscard]] uint8_t  samplePeakRange(double startSec, double endSec) const noexcept;
};

void setFfmpegSearchDirectory(std::filesystem::path directory);

[[nodiscard]] std::shared_ptr<DecodedAudioAsset const> loadAudioFileSync(std::string const& utf8Path);

[[nodiscard]] std::shared_ptr<DecodedAudioAsset const> getOrRequestAudioAsset(std::string const& utf8Path);

void clearAudioCache();

void shutdownAudioCache();

} // namespace playback::audio::external
