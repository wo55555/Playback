#pragma once

#include "playback/functions/render/FrameTap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace playback::editor::exporting::detail {

inline constexpr uint32_t MaxFrameDimension = 16384;
inline constexpr uint64_t MaxFrameBytes     = 512ull * 1024 * 1024;

[[nodiscard]] inline bool validateFrame(functions::render::CapturedFrame const& frame) {
    if (frame.width == 0 || frame.height == 0 || frame.width > MaxFrameDimension || frame.height > MaxFrameDimension) {
        return false;
    }
    uint64_t const minimumRowPitch = static_cast<uint64_t>(frame.width) * 4;
    uint64_t const requiredBytes   = static_cast<uint64_t>(frame.rowPitch) * frame.height;
    return frame.rowPitch >= minimumRowPitch && requiredBytes <= frame.pixels.size() && requiredBytes <= MaxFrameBytes
        && frame.ticket.ptsDenominator > 0
        && (frame.pixelFormat == functions::render::FramePixelFormat::Rgba8
            || frame.pixelFormat == functions::render::FramePixelFormat::Bgra8)
        && frame.colorSpace == functions::render::FrameColorSpace::SdrSrgb;
}

inline void copyPackedRgba(functions::render::CapturedFrame const& frame, std::vector<uint8_t>& rgba) {
    size_t const targetRowPitch = static_cast<size_t>(frame.width) * 4;
    rgba.resize(targetRowPitch * frame.height);
    auto const* sourcePixels = reinterpret_cast<uint8_t const*>(frame.pixels.data());
    for (uint32_t y = 0; y < frame.height; ++y) {
        auto const* sourceRow = sourcePixels + static_cast<size_t>(y) * frame.rowPitch;
        auto*       targetRow = rgba.data() + static_cast<size_t>(y) * targetRowPitch;
        if (frame.pixelFormat == functions::render::FramePixelFormat::Rgba8) {
            std::memcpy(targetRow, sourceRow, targetRowPitch);
            continue;
        }
        for (uint32_t x = 0; x < frame.width; ++x) {
            auto const* source = sourceRow + static_cast<size_t>(x) * 4;
            auto*       target = targetRow + static_cast<size_t>(x) * 4;
            target[0]          = source[2];
            target[1]          = source[1];
            target[2]          = source[0];
            target[3]          = source[3];
        }
    }
}

// Convert a captured frame to a stable output size. The renderer may expose
// the window size even after Bedrock has been asked to render at a supersampled
// size, so the export boundary owns this final, deterministic size contract.
[[nodiscard]] inline bool
normalizeFrame(functions::render::CapturedFrame& frame, uint32_t targetWidth, uint32_t targetHeight) {
    if (targetWidth == 0 && targetHeight == 0) return validateFrame(frame);
    if (targetWidth == 0 || targetHeight == 0 || targetWidth > MaxFrameDimension || targetHeight > MaxFrameDimension) {
        return false;
    }
    uint64_t const targetRowPitch = static_cast<uint64_t>(targetWidth) * 4;
    uint64_t const targetBytes    = targetRowPitch * targetHeight;
    if (targetBytes > MaxFrameBytes || targetBytes > std::numeric_limits<size_t>::max()) return false;
    if (!validateFrame(frame)) return false;

    // Keep the GPU readback intact when it already satisfies the output
    // contract. This is the common 1x path and avoids a full CPU bilinear pass
    // for every frame.
    if (frame.width == targetWidth && frame.height == targetHeight && frame.rowPitch == targetRowPitch
        && frame.pixelFormat == functions::render::FramePixelFormat::Rgba8) {
        return true;
    }

    auto const* source = reinterpret_cast<uint8_t const*>(frame.pixels.data());
    auto const  read   = [&](uint32_t x, uint32_t y, uint32_t channel) -> uint8_t {
        auto const* pixel = source + static_cast<size_t>(y) * frame.rowPitch + static_cast<size_t>(x) * 4;
        if (frame.pixelFormat == functions::render::FramePixelFormat::Bgra8) {
            static constexpr uint32_t channelMap[] = {2, 1, 0, 3};
            return pixel[channelMap[channel]];
        }
        return pixel[channel];
    };

    std::vector<std::byte> output(static_cast<size_t>(targetBytes));
    bool const             integerDownsample = frame.width % targetWidth == 0 && frame.height % targetHeight == 0
                                && frame.width / targetWidth == frame.height / targetHeight
                                && frame.width / targetWidth > 1;
    if (integerDownsample) {
        uint32_t const scale       = frame.width / targetWidth;
        uint64_t const sampleCount = static_cast<uint64_t>(scale) * scale;
        for (uint32_t y = 0; y < targetHeight; ++y) {
            for (uint32_t x = 0; x < targetWidth; ++x) {
                uint64_t sums[4]{};
                for (uint32_t sampleY = 0; sampleY < scale; ++sampleY) {
                    for (uint32_t sampleX = 0; sampleX < scale; ++sampleX) {
                        uint32_t const sourceX = x * scale + sampleX;
                        uint32_t const sourceY = y * scale + sampleY;
                        for (uint32_t channel = 0; channel < 4; ++channel) {
                            sums[channel] += read(sourceX, sourceY, channel);
                        }
                    }
                }

                auto* out = reinterpret_cast<uint8_t*>(output.data()) + (static_cast<size_t>(y) * targetWidth + x) * 4;
                for (uint32_t channel = 0; channel < 4; ++channel) {
                    out[channel] = static_cast<uint8_t>((sums[channel] + sampleCount / 2) / sampleCount);
                }
            }
        }
    } else {
        for (uint32_t y = 0; y < targetHeight; ++y) {
            double const   sourceY = (static_cast<double>(y) + 0.5) * frame.height / targetHeight - 0.5;
            uint32_t const y0 =
                static_cast<uint32_t>(std::clamp(std::floor(sourceY), 0.0, static_cast<double>(frame.height - 1)));
            uint32_t const y1 = std::min<uint32_t>(y0 + 1, frame.height - 1);
            double const   fy = std::clamp(sourceY - std::floor(sourceY), 0.0, 1.0);
            for (uint32_t x = 0; x < targetWidth; ++x) {
                double const   sourceX = (static_cast<double>(x) + 0.5) * frame.width / targetWidth - 0.5;
                uint32_t const x0 =
                    static_cast<uint32_t>(std::clamp(std::floor(sourceX), 0.0, static_cast<double>(frame.width - 1)));
                uint32_t const x1 = std::min<uint32_t>(x0 + 1, frame.width - 1);
                double const   fx = std::clamp(sourceX - std::floor(sourceX), 0.0, 1.0);
                auto* out = reinterpret_cast<uint8_t*>(output.data()) + (static_cast<size_t>(y) * targetWidth + x) * 4;
                for (uint32_t channel = 0; channel < 4; ++channel) {
                    double const top    = read(x0, y0, channel) * (1.0 - fx) + read(x1, y0, channel) * fx;
                    double const bottom = read(x0, y1, channel) * (1.0 - fx) + read(x1, y1, channel) * fx;
                    out[channel] =
                        static_cast<uint8_t>(std::clamp(std::lround(top * (1.0 - fy) + bottom * fy), 0l, 255l));
                }
            }
        }
    }

    frame.width       = targetWidth;
    frame.height      = targetHeight;
    frame.rowPitch    = static_cast<uint32_t>(targetRowPitch);
    frame.pixelFormat = functions::render::FramePixelFormat::Rgba8;
    frame.pixels      = std::move(output);
    return true;
}

} // namespace playback::editor::exporting::detail
