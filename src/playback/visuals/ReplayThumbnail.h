#pragma once

#include "FrameTap.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace playback::visuals {

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

class ReplayThumbnailLoader {
public:
    ReplayThumbnailLoader();
    ~ReplayThumbnailLoader();

    void                                                       beginFrame();
    void                                                       endFrame();
    void                                                       reset();
    void                                                       stop();
    [[nodiscard]] std::shared_ptr<ReplayThumbnailPixels const> request(std::filesystem::path const& path);

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

[[nodiscard]] bool writeRgbaPng(
    std::filesystem::path const& output,
    uint32_t                     width,
    uint32_t                     height,
    uint8_t const*               rgba,
    uint32_t                     rowPitch
);

[[nodiscard]] bool writeReplayThumbnailPng(
    std::filesystem::path const& output,
    uint32_t                     width,
    uint32_t                     height,
    uint8_t const*               rgba,
    uint32_t                     rowPitch
);

[[nodiscard]] bool writeReplayThumbnailPng(
    std::filesystem::path const& output,
    CapturedFrame const&         frame,
    uint32_t                     targetWidth,
    uint32_t                     targetHeight
);

[[nodiscard]] bool decodeReplayThumbnailPng(std::string_view png, ReplayThumbnailPixels& output);

} // namespace playback::visuals
