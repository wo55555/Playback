#pragma once

#include "FrameWorkerPool.h"

#include "playback/visuals/FrameCaptureTypes.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace playback::exporting::detail {

inline constexpr uint32_t MaxFrameDimension = 16384;
inline constexpr uint64_t MaxFrameBytes     = 512ull * 1024 * 1024;

[[nodiscard]] inline bool validateFrame(visuals::CapturedFrame const& frame) {
    if (frame.width == 0 || frame.height == 0 || frame.width > MaxFrameDimension || frame.height > MaxFrameDimension) {
        return false;
    }
    uint64_t const minimumRowPitch = static_cast<uint64_t>(frame.width) * 4;
    uint64_t const requiredBytes   = static_cast<uint64_t>(frame.rowPitch) * frame.height;
    return frame.rowPitch >= minimumRowPitch && requiredBytes <= frame.pixels.size() && requiredBytes <= MaxFrameBytes
        && frame.ticket.ptsDenominator > 0
        && (frame.pixelFormat == visuals::FramePixelFormat::Rgba8
            || frame.pixelFormat == visuals::FramePixelFormat::Bgra8)
        && frame.colorSpace == visuals::FrameColorSpace::SdrSrgb;
}

inline void copyPackedRgba(visuals::CapturedFrame const& frame, std::vector<uint8_t>& rgba) {
    size_t const targetRowPitch = static_cast<size_t>(frame.width) * 4;
    rgba.resize(targetRowPitch * frame.height);
    auto const* sourcePixels = reinterpret_cast<uint8_t const*>(frame.pixels.data());
    for (uint32_t y = 0; y < frame.height; ++y) {
        auto const* sourceRow = sourcePixels + static_cast<size_t>(y) * frame.rowPitch;
        auto*       targetRow = rgba.data() + static_cast<size_t>(y) * targetRowPitch;
        if (frame.pixelFormat == visuals::FramePixelFormat::Rgba8) {
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

[[nodiscard]] inline bool normalizeFrame(visuals::CapturedFrame& frame, uint32_t targetWidth, uint32_t targetHeight) {
    if (targetWidth == 0 && targetHeight == 0) return validateFrame(frame);
    if (targetWidth == 0 || targetHeight == 0 || targetWidth > MaxFrameDimension || targetHeight > MaxFrameDimension) {
        return false;
    }
    uint64_t const targetRowPitch = static_cast<uint64_t>(targetWidth) * 4;
    uint64_t const targetBytes    = targetRowPitch * targetHeight;
    if (targetBytes > MaxFrameBytes || targetBytes > std::numeric_limits<size_t>::max()) return false;
    if (!validateFrame(frame)) return false;

    if (frame.width == targetWidth && frame.height == targetHeight && frame.rowPitch == targetRowPitch
        && frame.pixelFormat == visuals::FramePixelFormat::Rgba8) {
        return true;
    }

    auto const* source = reinterpret_cast<uint8_t const*>(frame.pixels.data());
    auto const  read   = [&](uint32_t x, uint32_t y, uint32_t channel) -> uint8_t {
        auto const* pixel = source + static_cast<size_t>(y) * frame.rowPitch + static_cast<size_t>(x) * 4;
        if (frame.pixelFormat == visuals::FramePixelFormat::Bgra8) {
            static constexpr uint32_t channelMap[] = {2, 1, 0, 3};
            return pixel[channelMap[channel]];
        }
        return pixel[channel];
    };

    bool const sameSize          = frame.width == targetWidth && frame.height == targetHeight;
    bool const integerDownsample = !sameSize && frame.width % targetWidth == 0 && frame.height % targetHeight == 0
                                && frame.width / targetWidth == frame.height / targetHeight;
    // Present-time capture hands back the swap-chain size, which is rarely an integer multiple of the output,
    // so anything at least as large as the target goes through a general box filter instead.
    bool const boxDownsample =
        !sameSize && !integerDownsample && frame.width >= targetWidth && frame.height >= targetHeight;
    if (!sameSize && !integerDownsample && !boxDownsample) return false;

    std::vector<std::byte> output(static_cast<size_t>(targetBytes));
    if (integerDownsample) {
        // Folding the swizzle into the box filter leaves the later packed copy as a plain memcpy per row.
        uint32_t const scale       = frame.width / targetWidth;
        uint32_t const sampleCount = scale * scale;
        uint32_t const half        = sampleCount / 2;
        bool const     swapRedBlue = frame.pixelFormat == visuals::FramePixelFormat::Bgra8;
        uint32_t const redIndex    = swapRedBlue ? 2 : 0;
        uint32_t const blueIndex   = swapRedBlue ? 0 : 2;

        uint32_t shift = 0;
        while ((1u << shift) < sampleCount) ++shift;
        bool const powerOfTwo = (1u << shift) == sampleCount;

        if (scale == 2 && powerOfTwo) {
            std::function<void(uint32_t, uint32_t)> const rows = [&](uint32_t firstRow, uint32_t lastRow) {
                for (uint32_t y = firstRow; y < lastRow; ++y) {
                    auto* out = reinterpret_cast<uint8_t*>(output.data()) + static_cast<size_t>(y) * targetWidth * 4;
                    auto const* row0 = source + static_cast<size_t>(y) * 2 * frame.rowPitch;
                    auto const* row1 = row0 + frame.rowPitch;
                    for (uint32_t x = 0; x < targetWidth; ++x, out += 4, row0 += 8, row1 += 8) {
                        uint32_t const sums[4]{
                            static_cast<uint32_t>(row0[0]) + row0[4] + row1[0] + row1[4],
                            static_cast<uint32_t>(row0[1]) + row0[5] + row1[1] + row1[5],
                            static_cast<uint32_t>(row0[2]) + row0[6] + row1[2] + row1[6],
                            static_cast<uint32_t>(row0[3]) + row0[7] + row1[3] + row1[7],
                        };
                        out[0] = static_cast<uint8_t>((sums[redIndex] + 2) >> 2);
                        out[1] = static_cast<uint8_t>((sums[1] + 2) >> 2);
                        out[2] = static_cast<uint8_t>((sums[blueIndex] + 2) >> 2);
                        out[3] = static_cast<uint8_t>((sums[3] + 2) >> 2);
                    }
                }
            };
            frameWorkerPool().runRows(targetHeight, rows);
        } else {
            for (uint32_t y = 0; y < targetHeight; ++y) {
                auto*       out = reinterpret_cast<uint8_t*>(output.data()) + static_cast<size_t>(y) * targetWidth * 4;
                auto const* blockRow = source + static_cast<size_t>(y) * scale * frame.rowPitch;
                for (uint32_t x = 0; x < targetWidth; ++x, out += 4) {
                    uint32_t    sums[4]{};
                    auto const* block = blockRow + static_cast<size_t>(x) * scale * 4;
                    for (uint32_t sampleY = 0; sampleY < scale; ++sampleY) {
                        auto const* row = block + static_cast<size_t>(sampleY) * frame.rowPitch;
                        for (uint32_t sampleX = 0; sampleX < scale; ++sampleX, row += 4) {
                            sums[0] += row[0];
                            sums[1] += row[1];
                            sums[2] += row[2];
                            sums[3] += row[3];
                        }
                    }
                    if (powerOfTwo) {
                        out[0] = static_cast<uint8_t>((sums[redIndex] + half) >> shift);
                        out[1] = static_cast<uint8_t>((sums[1] + half) >> shift);
                        out[2] = static_cast<uint8_t>((sums[blueIndex] + half) >> shift);
                        out[3] = static_cast<uint8_t>((sums[3] + half) >> shift);
                    } else {
                        out[0] = static_cast<uint8_t>((sums[redIndex] + half) / sampleCount);
                        out[1] = static_cast<uint8_t>((sums[1] + half) / sampleCount);
                        out[2] = static_cast<uint8_t>((sums[blueIndex] + half) / sampleCount);
                        out[3] = static_cast<uint8_t>((sums[3] + half) / sampleCount);
                    }
                }
            }
        }
    } else if (boxDownsample) {
        // Each output pixel averages the source rectangle it maps to, so non-integer ratios stay artefact free.
        bool const     swapRedBlue = frame.pixelFormat == visuals::FramePixelFormat::Bgra8;
        uint32_t const redIndex    = swapRedBlue ? 2 : 0;
        uint32_t const blueIndex   = swapRedBlue ? 0 : 2;

        std::function<void(uint32_t, uint32_t)> const rows = [&](uint32_t firstRow, uint32_t lastRow) {
            for (uint32_t y = firstRow; y < lastRow; ++y) {
                auto const sourceTop = static_cast<uint64_t>(y) * frame.height / targetHeight;
                auto const sourceBottom =
                    std::max(sourceTop + 1, static_cast<uint64_t>(y + 1) * frame.height / targetHeight);
                auto* out = reinterpret_cast<uint8_t*>(output.data()) + static_cast<size_t>(y) * targetWidth * 4;
                for (uint32_t x = 0; x < targetWidth; ++x, out += 4) {
                    auto const sourceLeft = static_cast<uint64_t>(x) * frame.width / targetWidth;
                    auto const sourceRight =
                        std::max(sourceLeft + 1, static_cast<uint64_t>(x + 1) * frame.width / targetWidth);

                    uint32_t sums[4]{};
                    uint32_t samples{};
                    for (auto sourceY = sourceTop; sourceY < sourceBottom; ++sourceY) {
                        auto const* row = source + static_cast<size_t>(sourceY) * frame.rowPitch
                                        + static_cast<size_t>(sourceLeft) * 4;
                        for (auto sourceX = sourceLeft; sourceX < sourceRight; ++sourceX, row += 4) {
                            sums[0] += row[0];
                            sums[1] += row[1];
                            sums[2] += row[2];
                            sums[3] += row[3];
                            ++samples;
                        }
                    }
                    auto const half = samples / 2;
                    out[0]          = static_cast<uint8_t>((sums[redIndex] + half) / samples);
                    out[1]          = static_cast<uint8_t>((sums[1] + half) / samples);
                    out[2]          = static_cast<uint8_t>((sums[blueIndex] + half) / samples);
                    out[3]          = static_cast<uint8_t>((sums[3] + half) / samples);
                }
            }
        };
        frameWorkerPool().runRows(targetHeight, rows);
    } else {
        for (uint32_t y = 0; y < targetHeight; ++y) {
            for (uint32_t x = 0; x < targetWidth; ++x) {
                auto* out = reinterpret_cast<uint8_t*>(output.data()) + (static_cast<size_t>(y) * targetWidth + x) * 4;
                for (uint32_t channel = 0; channel < 4; ++channel) {
                    out[channel] = read(x, y, channel);
                }
            }
        }
    }

    frame.width       = targetWidth;
    frame.height      = targetHeight;
    frame.rowPitch    = static_cast<uint32_t>(targetRowPitch);
    frame.pixelFormat = visuals::FramePixelFormat::Rgba8;
    frame.pixels      = std::move(output);
    return true;
}

} // namespace playback::exporting::detail
