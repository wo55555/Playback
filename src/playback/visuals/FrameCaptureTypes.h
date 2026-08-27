#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace playback::visuals {

enum class FramePixelFormat : uint8_t { Rgba8, Bgra8 };
enum class FrameColorSpace : uint8_t { SdrSrgb };

struct FrameTicket {
    uint64_t frameIndex{};
    int64_t  ptsNumerator{};
    int64_t  ptsDenominator{1};
};

struct FrameSubmission {
    uint32_t         width{};
    uint32_t         height{};
    uint32_t         sampleCount{1};
    FramePixelFormat pixelFormat{FramePixelFormat::Rgba8};
    void*            exportResource{};
    void*            commandQueue{};
    void*            completionFence{};
    uint64_t         completionFenceValue{};
    uint32_t         sourceState{};
};

struct CapturedFrame {
    FrameTicket            ticket;
    FrameSubmission        submission;
    uint32_t               width{};
    uint32_t               height{};
    uint32_t               rowPitch{};
    FramePixelFormat       pixelFormat{FramePixelFormat::Rgba8};
    FrameColorSpace        colorSpace{FrameColorSpace::SdrSrgb};
    std::vector<std::byte> pixels;
};

} // namespace playback::visuals
