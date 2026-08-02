#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace playback::functions::render {

class ReplayThumbnailCaptureProvider {
public:
    virtual ~ReplayThumbnailCaptureProvider() = default;

    virtual void               requestReplayThumbnailCapture()                          = 0;
    [[nodiscard]] virtual bool saveReplayThumbnail(std::filesystem::path const& output) = 0;
};

struct ReplayThumbnailPixels {
    uint32_t             width{};
    uint32_t             height{};
    std::vector<uint8_t> rgba;
};

[[nodiscard]] bool writeReplayThumbnailPng(
    std::filesystem::path const& output,
    uint32_t                     width,
    uint32_t                     height,
    uint8_t const*               rgba,
    uint32_t                     rowPitch
);

[[nodiscard]] bool decodeReplayThumbnailPng(std::string_view png, ReplayThumbnailPixels& output);

} // namespace playback::functions::render
