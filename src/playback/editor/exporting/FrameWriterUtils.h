#pragma once

#include "playback/functions/render/FrameTap.h"

#include <cstdint>
#include <cstring>
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

} // namespace playback::editor::exporting::detail
