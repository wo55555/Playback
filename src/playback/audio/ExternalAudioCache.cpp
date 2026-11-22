#include "ExternalAudioCache.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace playback::audio::external {

namespace {

constexpr uint64_t MaxDecodedFrames = uint64_t{DecodedAudioAsset::SampleRate} * 1800ULL;
constexpr size_t   FramesPerBucket  = DecodedAudioAsset::SampleRate / DecodedAudioAsset::PeaksPerSecond;
constexpr size_t   WavChunkFrames   = 16384;

enum class WavSampleEncoding { Pcm, IeeeFloat };

struct WavFormat {
    WavSampleEncoding encoding{WavSampleEncoding::Pcm};
    uint16_t          numChannels{0};
    uint32_t          sampleRate{0};
    uint16_t          blockAlign{0};
    uint16_t          bitsPerSample{0};
};

enum class WavDecodeStatus { Ok, NotWav, UnsupportedForFfmpeg, Failed };

struct WavDecodeOutcome {
    WavDecodeStatus status{WavDecodeStatus::Failed};
    std::string     errorMessage;
};

float sanitizeSample(float value) noexcept {
    if (!std::isfinite(value)) return 0.0f;
    return std::clamp(value, -1.0f, 1.0f);
}

uint16_t readU16LE(uint8_t const* p) noexcept {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readU32LE(uint8_t const* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

int32_t readI24LE(uint8_t const* p) noexcept {
    uint32_t const raw =
        static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
    if ((raw & 0x00800000U) != 0U) {
        return static_cast<int32_t>(raw | 0xFF000000U);
    }
    return static_cast<int32_t>(raw);
}

float decodeSample(uint8_t const* ptr, WavSampleEncoding encoding, uint16_t bitsPerSample) noexcept {
    if (encoding == WavSampleEncoding::IeeeFloat) {
        if (bitsPerSample == 32) {
            float value = 0.0f;
            std::memcpy(&value, ptr, sizeof(value));
            return sanitizeSample(value);
        }
        return 0.0f;
    }

    switch (bitsPerSample) {
    case 8: {
        int const centered = static_cast<int>(ptr[0]) - 128;
        return std::clamp(static_cast<float>(centered) / 128.0f, -1.0f, 1.0f);
    }
    case 16: {
        int16_t const s = static_cast<int16_t>(readU16LE(ptr));
        return std::clamp(static_cast<float>(s) / 32768.0f, -1.0f, 1.0f);
    }
    case 24: {
        int32_t const s = readI24LE(ptr);
        return std::clamp(static_cast<float>(s) / 8388608.0f, -1.0f, 1.0f);
    }
    case 32: {
        int32_t const s = static_cast<int32_t>(readU32LE(ptr));
        return std::clamp(static_cast<float>(static_cast<double>(s) / 2147483648.0), -1.0f, 1.0f);
    }
    default:
        return 0.0f;
    }
}

void decodeRawFramesToStereo(
    uint8_t const*   rawBytes,
    size_t           frameCount,
    WavFormat const& fmt,
    float*           outStereo
) noexcept {
    size_t const bytesPerSample = fmt.bitsPerSample / 8;
    for (size_t i = 0; i < frameCount; ++i) {
        uint8_t const* framePtr = rawBytes + i * fmt.blockAlign;
        float const    left     = decodeSample(framePtr, fmt.encoding, fmt.bitsPerSample);
        float const    right =
            fmt.numChannels >= 2 ? decodeSample(framePtr + bytesPerSample, fmt.encoding, fmt.bitsPerSample) : left;
        outStereo[i * 2]     = left;
        outStereo[i * 2 + 1] = right;
    }
}

std::optional<std::filesystem::path> utf8ToPath(std::string const& utf8) {
    if (utf8.empty()) return std::nullopt;
    int const wideLen =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wideLen <= 0) return std::nullopt;
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            utf8.data(),
            static_cast<int>(utf8.size()),
            wide.data(),
            wideLen
        )
        <= 0) {
        return std::nullopt;
    }
    return std::filesystem::path(std::move(wide));
}

std::string pathToUtf8(std::filesystem::path const& path) {
    std::wstring const& wide = path.native();
    if (wide.empty()) return {};
    int const utf8Len =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return {};
    std::string utf8(static_cast<size_t>(utf8Len), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            0,
            wide.data(),
            static_cast<int>(wide.size()),
            utf8.data(),
            utf8Len,
            nullptr,
            nullptr
        )
        <= 0) {
        return {};
    }
    return utf8;
}

std::vector<uint8_t> buildPeakEnvelope(std::vector<float> const& pcmInterleaved) {
    size_t const totalFrames = pcmInterleaved.size() / DecodedAudioAsset::Channels;
    if (totalFrames == 0) return {};

    size_t const         bucketCount = (totalFrames + FramesPerBucket - 1) / FramesPerBucket;
    std::vector<uint8_t> peaks;
    peaks.reserve(bucketCount);

    for (size_t start = 0; start < totalFrames; start += FramesPerBucket) {
        size_t const end  = std::min(start + FramesPerBucket, totalFrames);
        float        peak = 0.0f;
        for (size_t f = start; f < end; ++f) {
            float const l = std::fabs(pcmInterleaved[f * 2]);
            float const r = std::fabs(pcmInterleaved[f * 2 + 1]);
            peak          = std::max(peak, std::max(l, r));
        }
        float const clamped = std::clamp(peak, 0.0f, 1.0f);
        peaks.push_back(static_cast<uint8_t>(std::clamp(std::lround(clamped * 255.0f), 0L, 255L)));
    }
    return peaks;
}

void finalizeDecodedAsset(DecodedAudioAsset& asset) {
    if ((asset.pcmInterleaved.size() % DecodedAudioAsset::Channels) != 0) {
        asset.pcmInterleaved.pop_back();
    }
    uint64_t const frames = asset.frameCount();
    asset.durationSeconds = static_cast<double>(frames) / static_cast<double>(DecodedAudioAsset::SampleRate);
    asset.peakEnvelope    = buildPeakEnvelope(asset.pcmInterleaved);
    asset.state           = AudioLoadState::Ready;
    asset.errorMessage.clear();
}

std::shared_ptr<DecodedAudioAsset> makeFailedAsset(std::string const& utf8Path, std::string errorMessage) {
    auto asset          = std::make_shared<DecodedAudioAsset>();
    asset->state        = AudioLoadState::Failed;
    asset->filePath     = utf8Path;
    asset->errorMessage = std::move(errorMessage);
    return asset;
}

WavDecodeOutcome tryDecodeWav(std::filesystem::path const& filePath, DecodedAudioAsset& asset) {
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {WavDecodeStatus::Failed, "Unable to open audio file"};
    }

    std::streamoff const endPos = file.tellg();
    if (endPos < 0) {
        return {WavDecodeStatus::Failed, "Unable to query audio file size"};
    }
    uint64_t const fileSize = static_cast<uint64_t>(endPos);
    if (fileSize < 12) {
        return {WavDecodeStatus::NotWav, "File is smaller than RIFF header"};
    }

    file.seekg(0, std::ios::beg);
    std::array<uint8_t, 12> riffHeader{};
    if (!file.read(reinterpret_cast<char*>(riffHeader.data()), static_cast<std::streamsize>(riffHeader.size()))) {
        return {WavDecodeStatus::Failed, "Failed to read file header"};
    }
    if (std::memcmp(riffHeader.data(), "RIFF", 4) != 0 || std::memcmp(riffHeader.data() + 8, "WAVE", 4) != 0) {
        return {WavDecodeStatus::NotWav, "Not a RIFF/WAVE container"};
    }

    bool      hasFmt      = false;
    bool      hasData     = false;
    bool      unsupported = false;
    WavFormat fmt{};
    uint64_t  dataOffset = 0;
    uint64_t  dataBytes  = 0;

    static constexpr std::array<uint8_t, 16> kPcmSubFormatGuid =
        {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    static constexpr std::array<uint8_t, 16> kFloatSubFormatGuid =
        {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

    uint64_t offset = 12;
    while (offset + 8 <= fileSize && !(hasFmt && hasData)) {
        file.clear();
        file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        std::array<uint8_t, 8> chunkHeader{};
        if (!file.read(reinterpret_cast<char*>(chunkHeader.data()), static_cast<std::streamsize>(chunkHeader.size()))) {
            return {WavDecodeStatus::Failed, "Truncated WAV chunk header"};
        }
        uint32_t const chunkSize     = readU32LE(chunkHeader.data() + 4);
        uint64_t const payloadOffset = offset + 8;
        uint64_t const remaining     = fileSize - payloadOffset;

        if (std::memcmp(chunkHeader.data(), "fmt ", 4) == 0) {
            if (chunkSize < 16 || chunkSize > remaining) {
                return {WavDecodeStatus::Failed, "Invalid WAV fmt chunk"};
            }
            std::array<uint8_t, 40> fmtBytes{};
            size_t const            toRead = std::min<size_t>(chunkSize, fmtBytes.size());
            if (!file.read(reinterpret_cast<char*>(fmtBytes.data()), static_cast<std::streamsize>(toRead))) {
                return {WavDecodeStatus::Failed, "Failed to read WAV fmt chunk"};
            }

            uint16_t const audioFormat = readU16LE(fmtBytes.data() + 0);
            fmt.numChannels            = readU16LE(fmtBytes.data() + 2);
            fmt.sampleRate             = readU32LE(fmtBytes.data() + 4);
            fmt.blockAlign             = readU16LE(fmtBytes.data() + 12);
            fmt.bitsPerSample          = readU16LE(fmtBytes.data() + 14);

            if (audioFormat == 1) {
                fmt.encoding = WavSampleEncoding::Pcm;
            } else if (audioFormat == 3) {
                fmt.encoding = WavSampleEncoding::IeeeFloat;
            } else if (audioFormat == 0xFFFE) {
                if (chunkSize < 40) {
                    return {WavDecodeStatus::Failed, "Truncated extensible WAV fmt chunk"};
                }
                uint16_t const cbSize = readU16LE(fmtBytes.data() + 16);
                if (cbSize < 22) {
                    return {WavDecodeStatus::Failed, "Invalid extensible WAV cbSize"};
                }
                if (std::memcmp(fmtBytes.data() + 24, kPcmSubFormatGuid.data(), 16) == 0) {
                    fmt.encoding = WavSampleEncoding::Pcm;
                } else if (std::memcmp(fmtBytes.data() + 24, kFloatSubFormatGuid.data(), 16) == 0) {
                    fmt.encoding = WavSampleEncoding::IeeeFloat;
                } else {
                    unsupported = true;
                }
            } else {
                unsupported = true;
            }
            hasFmt = true;
        } else if (std::memcmp(chunkHeader.data(), "data", 4) == 0) {
            dataOffset = payloadOffset;
            dataBytes  = std::min<uint64_t>(chunkSize, remaining);
            hasData    = true;
        }

        uint64_t const paddedSize = uint64_t{chunkSize} + (uint64_t{chunkSize} & 1ULL);
        if (paddedSize > remaining) {
            break;
        }
        offset = payloadOffset + paddedSize;
    }

    if (!hasFmt || !hasData) {
        return {WavDecodeStatus::Failed, "Missing fmt or data chunk in WAV"};
    }
    if (fmt.numChannels == 0 || fmt.sampleRate == 0 || fmt.blockAlign == 0 || fmt.bitsPerSample == 0) {
        return {WavDecodeStatus::Failed, "Invalid WAV stream parameters"};
    }
    if (unsupported) {
        return {WavDecodeStatus::UnsupportedForFfmpeg, "Unsupported WAV codec"};
    }
    if (fmt.encoding == WavSampleEncoding::Pcm) {
        if (fmt.bitsPerSample != 8 && fmt.bitsPerSample != 16 && fmt.bitsPerSample != 24 && fmt.bitsPerSample != 32) {
            return {WavDecodeStatus::UnsupportedForFfmpeg, "Unsupported WAV PCM bit depth"};
        }
    } else if (fmt.bitsPerSample != 32) {
        return {WavDecodeStatus::UnsupportedForFfmpeg, "Unsupported WAV IEEE float bit depth"};
    }

    uint32_t const minBlockAlign = uint32_t{fmt.numChannels} * (uint32_t{fmt.bitsPerSample} / 8U);
    if (fmt.blockAlign < minBlockAlign) {
        return {WavDecodeStatus::Failed, "Invalid WAV block alignment"};
    }

    uint64_t const inFrames = dataBytes / fmt.blockAlign;
    if (inFrames == 0) {
        finalizeDecodedAsset(asset);
        return {WavDecodeStatus::Ok, {}};
    }

    uint64_t outFrames = inFrames;
    if (fmt.sampleRate != DecodedAudioAsset::SampleRate) {
        double const ratio  = static_cast<double>(DecodedAudioAsset::SampleRate) / static_cast<double>(fmt.sampleRate);
        double const estOut = static_cast<double>(inFrames) * ratio;
        if (!std::isfinite(estOut) || estOut > static_cast<double>(MaxDecodedFrames)) {
            return {WavDecodeStatus::Failed, "Audio duration exceeds maximum decoded frame limit"};
        }
        outFrames = static_cast<uint64_t>(std::llround(estOut));
        if (outFrames == 0) outFrames = 1;
    }
    if (outFrames > MaxDecodedFrames) {
        return {WavDecodeStatus::Failed, "Audio duration exceeds maximum decoded frame limit"};
    }

    size_t const chunkFrameCap = std::max<size_t>(2, std::min<size_t>(WavChunkFrames, 1048576 / fmt.blockAlign));
    asset.pcmInterleaved.resize(static_cast<size_t>(outFrames * DecodedAudioAsset::Channels));

    std::vector<uint8_t> rawBuf(chunkFrameCap * fmt.blockAlign);

    if (fmt.sampleRate == DecodedAudioAsset::SampleRate) {
        file.clear();
        file.seekg(static_cast<std::streamoff>(dataOffset), std::ios::beg);
        uint64_t framesLeft  = inFrames;
        size_t   outFramePos = 0;
        while (framesLeft > 0) {
            size_t const chunkFrames = static_cast<size_t>(std::min<uint64_t>(framesLeft, chunkFrameCap));
            size_t const chunkBytes  = chunkFrames * fmt.blockAlign;
            if (!file.read(reinterpret_cast<char*>(rawBuf.data()), static_cast<std::streamsize>(chunkBytes))) {
                asset.pcmInterleaved.clear();
                return {WavDecodeStatus::Failed, "Unexpected EOF reading WAV audio data"};
            }
            decodeRawFramesToStereo(
                rawBuf.data(),
                chunkFrames,
                fmt,
                asset.pcmInterleaved.data() + outFramePos * DecodedAudioAsset::Channels
            );
            outFramePos += chunkFrames;
            framesLeft  -= chunkFrames;
        }
    } else {
        std::vector<float> srcChunkStereo(chunkFrameCap * DecodedAudioAsset::Channels);
        uint64_t           chunkBase  = 0;
        size_t             chunkCount = 0;

        auto loadChunkAt = [&](uint64_t startFrame) -> bool {
            size_t const toRead = static_cast<size_t>(std::min<uint64_t>(chunkFrameCap, inFrames - startFrame));
            if (toRead == 0) return false;
            file.clear();
            file.seekg(static_cast<std::streamoff>(dataOffset + startFrame * fmt.blockAlign), std::ios::beg);
            size_t const bytesToRead = toRead * fmt.blockAlign;
            if (!file.read(reinterpret_cast<char*>(rawBuf.data()), static_cast<std::streamsize>(bytesToRead))) {
                return false;
            }
            decodeRawFramesToStereo(rawBuf.data(), toRead, fmt, srcChunkStereo.data());
            chunkBase  = startFrame;
            chunkCount = toRead;
            return true;
        };

        if (!loadChunkAt(0)) {
            asset.pcmInterleaved.clear();
            return {WavDecodeStatus::Failed, "Unexpected EOF reading WAV audio data"};
        }

        double const step = static_cast<double>(fmt.sampleRate) / static_cast<double>(DecodedAudioAsset::SampleRate);
        for (uint64_t outIdx = 0; outIdx < outFrames; ++outIdx) {
            double const   srcPos = static_cast<double>(outIdx) * step;
            uint64_t const idx0   = std::min(static_cast<uint64_t>(srcPos), inFrames - 1);
            uint64_t const idx1   = std::min(idx0 + 1, inFrames - 1);

            if (idx1 >= chunkBase + chunkCount) {
                if (!loadChunkAt(idx0)) {
                    asset.pcmInterleaved.clear();
                    return {WavDecodeStatus::Failed, "Unexpected EOF resampling WAV audio data"};
                }
            }

            size_t const local0 = static_cast<size_t>(idx0 - chunkBase);
            size_t const local1 = static_cast<size_t>(idx1 - chunkBase);
            float const  frac   = static_cast<float>(srcPos - static_cast<double>(idx0));

            float const l0                       = srcChunkStereo[local0 * 2];
            float const r0                       = srcChunkStereo[local0 * 2 + 1];
            float const l1                       = srcChunkStereo[local1 * 2];
            float const r1                       = srcChunkStereo[local1 * 2 + 1];
            asset.pcmInterleaved[outIdx * 2]     = sanitizeSample(l0 + (l1 - l0) * frac);
            asset.pcmInterleaved[outIdx * 2 + 1] = sanitizeSample(r0 + (r1 - r0) * frac);
        }
    }

    finalizeDecodedAsset(asset);
    return {WavDecodeStatus::Ok, {}};
}

std::mutex            gFfmpegDirMutex;
std::filesystem::path gFfmpegSearchDir;

std::filesystem::path locateFfmpeg() {
    std::filesystem::path searchDir;
    {
        std::lock_guard lock(gFfmpegDirMutex);
        searchDir = gFfmpegSearchDir;
    }

    std::error_code ec;
    if (!searchDir.empty()) {
        auto const inTools = searchDir / "tools" / "ffmpeg.exe";
        if (std::filesystem::is_regular_file(inTools, ec)) return inTools;
        auto const direct = searchDir / "ffmpeg.exe";
        if (std::filesystem::is_regular_file(direct, ec)) return direct;
    }

    DWORD const required = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, 0, nullptr, nullptr);
    if (required == 0) return {};
    std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1);
    DWORD const          copied =
        SearchPathW(nullptr, L"ffmpeg.exe", nullptr, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (copied == 0 || copied >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), copied));
}

std::wstring quoteWinArg(std::wstring const& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring result      = L"\"";
    size_t       backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(c);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::optional<std::string> decodeViaFfmpeg(std::filesystem::path const& filePath, DecodedAudioAsset& asset) {
    auto const ffmpeg = locateFfmpeg();
    if (ffmpeg.empty()) {
        return "ffmpeg.exe not found for non-WAV audio decoding";
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdoutRead  = nullptr;
    HANDLE stdoutWrite = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0)) {
        return "Failed to create stdout pipe for ffmpeg";
    }
    if (!SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);
        return "Failed to configure stdout pipe inheritance";
    }

    HANDLE nulHandle = CreateFileW(
        L"NUL",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    STARTUPINFOW startup{};
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdInput  = nulHandle != INVALID_HANDLE_VALUE ? nulHandle : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = stdoutWrite;
    startup.hStdError  = nulHandle != INVALID_HANDLE_VALUE ? nulHandle : GetStdHandle(STD_ERROR_HANDLE);

    std::wstring command  = quoteWinArg(ffmpeg.wstring());
    command              += L" -v error -nostdin -i " + quoteWinArg(filePath.wstring());
    command              += L" -vn -sn -dn -f f32le -ac 2 -ar 48000 pipe:1";

    std::vector<wchar_t> cmdBuf(command.begin(), command.end());
    cmdBuf.push_back(L'\0');

    PROCESS_INFORMATION processInfo{};
    BOOL const          created = CreateProcessW(
        ffmpeg.c_str(),
        cmdBuf.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &processInfo
    );

    CloseHandle(stdoutWrite);
    if (nulHandle != INVALID_HANDLE_VALUE) CloseHandle(nulHandle);

    if (!created) {
        CloseHandle(stdoutRead);
        return "Failed to launch ffmpeg process";
    }

    std::array<uint8_t, 65536> readBuf{};
    std::array<uint8_t, 4>     tail{};
    size_t                     tailCount     = 0;
    bool                       limitExceeded = false;
    bool                       readFailed    = false;

    for (;;) {
        DWORD      bytesRead = 0;
        BOOL const ok = ReadFile(stdoutRead, readBuf.data(), static_cast<DWORD>(readBuf.size()), &bytesRead, nullptr);
        if (!ok) {
            DWORD const err = GetLastError();
            if (err != ERROR_BROKEN_PIPE) {
                readFailed = true;
            }
            break;
        }
        if (bytesRead == 0) break;

        size_t offset = 0;
        if (tailCount > 0) {
            size_t const needed = std::min<size_t>(4 - tailCount, bytesRead);
            std::memcpy(tail.data() + tailCount, readBuf.data(), needed);
            tailCount += needed;
            offset    += needed;
            if (tailCount == 4) {
                float sample = 0.0f;
                std::memcpy(&sample, tail.data(), sizeof(sample));
                asset.pcmInterleaved.push_back(sanitizeSample(sample));
                tailCount = 0;
            }
        }

        size_t const remaining     = bytesRead - offset;
        size_t const alignedFloats = remaining / 4;
        for (size_t i = 0; i < alignedFloats; ++i) {
            float sample = 0.0f;
            std::memcpy(&sample, readBuf.data() + offset + i * 4, sizeof(sample));
            asset.pcmInterleaved.push_back(sanitizeSample(sample));
        }

        size_t const leftover = remaining % 4;
        if (leftover > 0) {
            std::memcpy(tail.data(), readBuf.data() + offset + alignedFloats * 4, leftover);
            tailCount = leftover;
        }

        if (asset.pcmInterleaved.size() / DecodedAudioAsset::Channels > MaxDecodedFrames) {
            limitExceeded = true;
            TerminateProcess(processInfo.hProcess, 1);
            break;
        }
    }

    CloseHandle(stdoutRead);

    DWORD const waitResult = WaitForSingleObject(processInfo.hProcess, 30'000);
    DWORD       exitCode   = 1;
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(processInfo.hProcess, &exitCode);
    } else {
        TerminateProcess(processInfo.hProcess, 1);
    }
    CloseHandle(processInfo.hProcess);
    CloseHandle(processInfo.hThread);

    if (limitExceeded) {
        asset.pcmInterleaved.clear();
        return "Audio duration exceeds maximum decoded frame limit";
    }
    if (readFailed) {
        asset.pcmInterleaved.clear();
        return "Error reading decoded PCM stream from ffmpeg";
    }
    if (waitResult != WAIT_OBJECT_0) {
        asset.pcmInterleaved.clear();
        return "ffmpeg decode timed out";
    }
    if (exitCode != 0) {
        asset.pcmInterleaved.clear();
        return "ffmpeg failed to decode audio file (exit code " + std::to_string(exitCode) + ")";
    }

    finalizeDecodedAsset(asset);
    return std::nullopt;
}

struct CacheKey {
    std::string normalizedPath;
    bool        exists{false};
    uint64_t    fileSize{0};
    int64_t     mtimeTicks{0};

    bool operator==(CacheKey const&) const noexcept = default;
};

struct CacheKeyHash {
    size_t operator()(CacheKey const& key) const noexcept {
        size_t h  = std::hash<std::string>{}(key.normalizedPath);
        h        ^= std::hash<bool>{}(key.exists) + 0x9e3779b9U + (h << 6) + (h >> 2);
        h        ^= std::hash<uint64_t>{}(key.fileSize) + 0x9e3779b9U + (h << 6) + (h >> 2);
        h        ^= std::hash<int64_t>{}(key.mtimeTicks) + 0x9e3779b9U + (h << 6) + (h >> 2);
        return h;
    }
};

CacheKey buildCacheKey(std::string const& utf8Path) {
    CacheKey   key;
    auto const maybePath = utf8ToPath(utf8Path);
    if (!maybePath) {
        key.normalizedPath = utf8Path;
        return key;
    }

    std::error_code ec;
    auto            absPath = std::filesystem::absolute(*maybePath, ec);
    auto            norm    = (ec ? *maybePath : absPath).lexically_normal();
    key.normalizedPath      = pathToUtf8(norm);
    if (key.normalizedPath.empty()) key.normalizedPath = utf8Path;

    if (std::filesystem::is_regular_file(norm, ec)) {
        key.exists   = true;
        auto const s = std::filesystem::file_size(norm, ec);
        if (!ec) key.fileSize = s;
        auto const t = std::filesystem::last_write_time(norm, ec);
        if (!ec) key.mtimeTicks = t.time_since_epoch().count();
    }
    return key;
}

struct DecodeTask {
    CacheKey    key;
    std::string utf8Path;
    uint64_t    generation{0};
};

class AudioCacheManager {
public:
    ~AudioCacheManager() { shutdown(); }

    std::shared_ptr<DecodedAudioAsset const> getOrRequest(std::string const& utf8Path) {
        CacheKey const key = buildCacheKey(utf8Path);

        std::lock_guard lock(mMutex);
        if (auto it = mCache.find(key); it != mCache.end()) {
            return it->second;
        }

        auto pending      = std::make_shared<DecodedAudioAsset>();
        pending->state    = AudioLoadState::Pending;
        pending->filePath = utf8Path;
        mCache.emplace(key, pending);

        if (!mWorker.joinable()) {
            mStopRequested = false;
            mWorker        = std::thread(&AudioCacheManager::workerLoop, this);
        }

        mQueue.push_back(DecodeTask{key, utf8Path, mGeneration});
        mCv.notify_one();
        return pending;
    }

    void clear() {
        std::lock_guard lock(mMutex);
        ++mGeneration;
        mQueue.clear();
        mCache.clear();
    }

    void shutdown() {
        std::thread threadToJoin;
        {
            std::lock_guard lock(mMutex);
            mStopRequested = true;
            ++mGeneration;
            mQueue.clear();
            mCache.clear();
            threadToJoin = std::move(mWorker);
        }
        mCv.notify_all();
        if (threadToJoin.joinable()) {
            threadToJoin.join();
        }
    }

private:
    void workerLoop() {
        for (;;) {
            DecodeTask task;
            {
                std::unique_lock lock(mMutex);
                mCv.wait(lock, [this]() { return mStopRequested || !mQueue.empty(); });
                if (mStopRequested) return;
                task = std::move(mQueue.front());
                mQueue.pop_front();
            }

            auto decoded = loadAudioFileSync(task.utf8Path);

            std::lock_guard lock(mMutex);
            if (mStopRequested || task.generation != mGeneration) {
                continue;
            }
            if (auto it = mCache.find(task.key); it != mCache.end()) {
                it->second = std::move(decoded);
            }
        }
    }

    std::mutex                                                                           mMutex;
    std::condition_variable                                                              mCv;
    std::unordered_map<CacheKey, std::shared_ptr<DecodedAudioAsset const>, CacheKeyHash> mCache;
    std::deque<DecodeTask>                                                               mQueue;
    std::thread                                                                          mWorker;
    uint64_t                                                                             mGeneration{0};
    bool                                                                                 mStopRequested{false};
};

AudioCacheManager& cacheManager() {
    static AudioCacheManager instance;
    return instance;
}

} // namespace

uint8_t DecodedAudioAsset::samplePeakRange(double startSec, double endSec) const noexcept {
    if (peakEnvelope.empty() || !std::isfinite(startSec) || !std::isfinite(endSec)) return 0;
    if (endSec < startSec) return 0;

    double const maxSec = static_cast<double>(peakEnvelope.size()) / static_cast<double>(PeaksPerSecond);
    double const s      = std::clamp(startSec, 0.0, maxSec);
    double const e      = std::clamp(endSec, s, maxSec);

    size_t const maxIdx = peakEnvelope.size() - 1;
    size_t const first  = std::min(static_cast<size_t>(std::floor(s * static_cast<double>(PeaksPerSecond))), maxIdx);
    size_t       lastEx = static_cast<size_t>(std::ceil(e * static_cast<double>(PeaksPerSecond)));
    if (lastEx <= first) lastEx = first + 1;
    lastEx = std::min(lastEx, peakEnvelope.size());

    uint8_t peak = 0;
    for (size_t i = first; i < lastEx; ++i) {
        peak = std::max(peak, peakEnvelope[i]);
    }
    return peak;
}

void setFfmpegSearchDirectory(std::filesystem::path directory) {
    std::lock_guard lock(gFfmpegDirMutex);
    gFfmpegSearchDir = std::move(directory);
}

std::shared_ptr<DecodedAudioAsset const> loadAudioFileSync(std::string const& utf8Path) {
    auto const maybePath = utf8ToPath(utf8Path);
    if (!maybePath) {
        return makeFailedAsset(utf8Path, "Invalid UTF-8 audio file path");
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(*maybePath, ec)) {
        return makeFailedAsset(utf8Path, "Audio file does not exist");
    }

    auto asset      = std::make_shared<DecodedAudioAsset>();
    asset->filePath = utf8Path;

    WavDecodeOutcome const wavResult = tryDecodeWav(*maybePath, *asset);
    if (wavResult.status == WavDecodeStatus::Ok) {
        return asset;
    }
    if (wavResult.status == WavDecodeStatus::Failed) {
        return makeFailedAsset(utf8Path, wavResult.errorMessage);
    }

    if (auto ffmpegErr = decodeViaFfmpeg(*maybePath, *asset); ffmpegErr.has_value()) {
        return makeFailedAsset(utf8Path, std::move(*ffmpegErr));
    }
    return asset;
}

std::shared_ptr<DecodedAudioAsset const> getOrRequestAudioAsset(std::string const& utf8Path) {
    return cacheManager().getOrRequest(utf8Path);
}

void clearAudioCache() { cacheManager().clear(); }

void shutdownAudioCache() { cacheManager().shutdown(); }

} // namespace playback::audio::external
