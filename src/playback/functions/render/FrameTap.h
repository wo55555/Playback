#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace playback::functions::render {

enum class FramePixelFormat : uint8_t { Rgba8, Bgra8 };

enum class FrameColorSpace : uint8_t { SdrSrgb };

enum class FrameTapState : uint8_t { Idle, Active, Completed, Cancelled, Faulted };

enum class FrameTapError : uint8_t {
    None,
    BackendUnavailable,
    UnsupportedFormat,
    Resize,
    DeviceLost,
    FenceFailed,
    MapFailed,
    Cancelled,
};

enum class FrameTapOpenResult : uint8_t { Opened, Busy, InvalidConfig };

enum class FrameTapArmResult : uint8_t { Armed, Busy, Backpressured, Inactive, InvalidTicket };

struct FrameTapConfig {
    uint32_t capacity{4};
    bool     oneShot{};
};

struct FrameTapSession {
    uint64_t id{};

    [[nodiscard]] explicit operator bool() const { return id != 0; }
};

struct FrameTicket {
    uint64_t frameIndex{};
    int64_t  ptsNumerator{};
    int64_t  ptsDenominator{1};
};

struct CapturedFrame {
    FrameTicket            ticket;
    uint32_t               width{};
    uint32_t               height{};
    uint32_t               rowPitch{};
    FramePixelFormat       pixelFormat{FramePixelFormat::Rgba8};
    FrameColorSpace        colorSpace{FrameColorSpace::SdrSrgb};
    std::vector<std::byte> pixels;
};

struct FrameTapStatus {
    FrameTapState state{FrameTapState::Idle};
    FrameTapError error{FrameTapError::None};
    std::string   message;
    uint32_t      bufferedFrames{};
    uint32_t      inFlightFrames{};
    bool          armed{};
};

struct FrameTapBackendCapture {
    FrameTapSession session;
    FrameTicket     ticket;
    uint64_t        captureId{};
};

class FrameTap {
public:
    FrameTap()                           = default;
    FrameTap(FrameTap const&)            = delete;
    FrameTap& operator=(FrameTap const&) = delete;

    FrameTapOpenResult open(FrameTapConfig config, FrameTapSession& session);
    void               close(FrameTapSession session);
    void               cancel(FrameTapSession session);

    [[nodiscard]] FrameTapArmResult                     tryArm(FrameTapSession session, FrameTicket ticket);
    [[nodiscard]] std::optional<FrameTapBackendCapture> tryPopStarted(FrameTapSession session);
    [[nodiscard]] std::optional<CapturedFrame>          tryPop(FrameTapSession session);
    [[nodiscard]] std::optional<CapturedFrame> waitPop(FrameTapSession session, std::chrono::milliseconds timeout);
    [[nodiscard]] FrameTapStatus               status(FrameTapSession session) const;

    [[nodiscard]] bool     requiresRenderPass() const;
    [[nodiscard]] uint32_t captureCapacity() const;

    // Graphics backends call these methods; they never block the Present thread.
    [[nodiscard]] std::optional<FrameTapBackendCapture> beginCapture();
    void                                                complete(FrameTapBackendCapture capture, CapturedFrame frame);
    void fail(FrameTapBackendCapture capture, FrameTapError error, std::string message);
    void failActive(FrameTapError error, std::string message);

private:
    struct ActiveSession {
        FrameTapSession                    handle;
        FrameTapConfig                     config;
        FrameTapState                      state{FrameTapState::Active};
        FrameTapError                      error{FrameTapError::None};
        std::string                        message;
        std::optional<FrameTicket>         armedTicket;
        std::deque<FrameTapBackendCapture> startedCaptures;
        std::deque<CapturedFrame>          readyFrames;
        uint32_t                           inFlightFrames{};
        uint64_t                           submittedFrames{};
    };

    [[nodiscard]] bool                         matches(FrameTapSession session) const;
    [[nodiscard]] std::optional<CapturedFrame> popReadyFrame();
    void                                       faultActive(FrameTapError error, std::string message);

    mutable std::mutex           mMutex;
    std::condition_variable      mChanged;
    std::optional<ActiveSession> mActive;
    uint64_t                     mNextSessionId{1};
    uint64_t                     mNextCaptureId{1};
};

} // namespace playback::functions::render
