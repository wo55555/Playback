#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>

namespace playback::audio::exportcap {

// Streaming 32-bit float stereo WAV writer. No FMOD dependency; pure file IO.
class WavWriter {
public:
    static constexpr uint32_t SampleRate    = 48000;
    static constexpr uint16_t Channels      = 2;
    static constexpr uint16_t BitsPerSample = 32;

    WavWriter() = default;
    ~WavWriter();

    WavWriter(WavWriter const&)            = delete;
    WavWriter& operator=(WavWriter const&) = delete;

    [[nodiscard]] bool open(std::filesystem::path const& path);
    // Append interleaved stereo float samples. count is the number of floats (samples*2).
    void append(float const* data, size_t count);
    // Pad with silence up to totalSamplesPerChannel samples per channel, to match the video length exactly.
    void padTo(uint64_t totalSamplesPerChannel);
    // Backfill RIFF/data sizes and close. Idempotent.
    void finalize();

    [[nodiscard]] bool     isOpen() const { return mFile.is_open(); }
    [[nodiscard]] uint64_t samplesPerChannel() const { return mSamplesPerChannel; }

private:
    std::ofstream mFile;
    uint64_t      mSamplesPerChannel{};
    uint32_t      mDataBytes{};
    bool          mFinalized{};
};

} // namespace playback::audio::exportcap
