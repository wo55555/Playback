#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>

namespace playback::audio::exportcap {

// Streaming 32-bit float WAV writer, mono or stereo. No FMOD dependency; pure file IO.
class WavWriter {
public:
    static constexpr uint32_t SampleRate    = 48000;
    static constexpr uint16_t MaxChannels   = 2;
    static constexpr uint16_t BitsPerSample = 32;

    WavWriter() = default;
    ~WavWriter();

    WavWriter(WavWriter const&)            = delete;
    WavWriter& operator=(WavWriter const&) = delete;

    // channels is clamped to 1 or 2.
    [[nodiscard]] bool open(std::filesystem::path const& path, uint16_t channels = MaxChannels);
    // Append interleaved float samples. count is the number of floats (samples * channels).
    void append(float const* data, size_t count);
    // Pad with silence up to totalSamplesPerChannel samples per channel, to match the video length exactly.
    void padTo(uint64_t totalSamplesPerChannel);
    // Backfill RIFF/data sizes and close. Idempotent.
    void finalize();

    [[nodiscard]] bool     isOpen() const { return mFile.is_open(); }
    [[nodiscard]] uint16_t channels() const { return mChannels; }
    [[nodiscard]] uint64_t samplesPerChannel() const { return mSamplesPerChannel; }

private:
    std::ofstream mFile;
    uint64_t      mSamplesPerChannel{};
    uint32_t      mDataBytes{};
    uint16_t      mChannels{MaxChannels};
    bool          mFinalized{};
};

} // namespace playback::audio::exportcap
