#include "ExportAudioWav.h"

#include <cstring>

namespace playback::audio::exportcap {

namespace {

#pragma pack(push, 1)
struct WavHeader {
    char     riff[4]; // "RIFF"
    uint32_t riffSize;
    char     wave[4];     // "WAVE"
    char     fmt[4];      // "fmt "
    uint32_t fmtSize;     // 16
    uint16_t audioFormat; // 3 = IEEE float
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char     data[4]; // "data"
    uint32_t dataSize;
};
#pragma pack(pop)

static_assert(sizeof(WavHeader) == 44, "WAV header must be 44 bytes");

} // namespace

WavWriter::~WavWriter() { finalize(); }

bool WavWriter::open(std::filesystem::path const& path) {
    finalize();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    mFile.open(path, std::ios::binary | std::ios::trunc);
    if (!mFile.is_open()) return false;

    WavHeader header{};
    std::memcpy(header.riff, "RIFF", 4);
    std::memcpy(header.wave, "WAVE", 4);
    std::memcpy(header.fmt, "fmt ", 4);
    header.fmtSize       = 16;
    header.audioFormat   = 3; // IEEE float
    header.numChannels   = Channels;
    header.sampleRate    = SampleRate;
    header.byteRate      = SampleRate * Channels * (BitsPerSample / 8);
    header.blockAlign    = Channels * (BitsPerSample / 8);
    header.bitsPerSample = BitsPerSample;
    std::memcpy(header.data, "data", 4);
    // riffSize/dataSize are backfilled in finalize().
    mFile.write(reinterpret_cast<char const*>(&header), sizeof(header));

    mSamplesPerChannel = 0;
    mDataBytes         = 0;
    mFinalized         = false;
    return mFile.good();
}

void WavWriter::append(float const* data, size_t count) {
    if (!mFile.is_open() || mFinalized || !data || count == 0) return;
    size_t const bytes = count * sizeof(float);
    mFile.write(reinterpret_cast<char const*>(data), static_cast<std::streamsize>(bytes));
    mDataBytes         += static_cast<uint32_t>(bytes);
    mSamplesPerChannel += count / Channels;
}

void WavWriter::padTo(uint64_t totalSamplesPerChannel) {
    if (!mFile.is_open() || mFinalized) return;
    if (totalSamplesPerChannel <= mSamplesPerChannel) return;
    uint64_t const         missing         = totalSamplesPerChannel - mSamplesPerChannel;
    static constexpr float zeros[1024 * 2] = {};
    uint64_t               remaining       = missing;
    while (remaining > 0) {
        uint64_t const chunkFrames = remaining > 1024 ? 1024 : remaining;
        append(zeros, static_cast<size_t>(chunkFrames * Channels));
        remaining -= chunkFrames;
    }
}

void WavWriter::finalize() {
    if (!mFile.is_open() || mFinalized) return;
    mFinalized              = true;
    uint32_t const riffSize = 36 + mDataBytes;
    mFile.seekp(4, std::ios::beg);
    mFile.write(reinterpret_cast<char const*>(&riffSize), sizeof(riffSize));
    mFile.seekp(40, std::ios::beg);
    mFile.write(reinterpret_cast<char const*>(&mDataBytes), sizeof(mDataBytes));
    mFile.close();
}

} // namespace playback::audio::exportcap
