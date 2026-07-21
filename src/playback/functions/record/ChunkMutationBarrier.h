#pragma once

#include <chrono>

class MultiPlayerLevel;

namespace playback::functions {

class ChunkMutationBarrier {
public:
    class TickBoundaryGuard {
    public:
        TickBoundaryGuard(TickBoundaryGuard const&)            = delete;
        TickBoundaryGuard& operator=(TickBoundaryGuard const&) = delete;
        TickBoundaryGuard(TickBoundaryGuard&&)                 = delete;
        TickBoundaryGuard& operator=(TickBoundaryGuard&&)      = delete;

        ~TickBoundaryGuard() noexcept;

    private:
        friend class ChunkMutationBarrier;

        explicit TickBoundaryGuard(MultiPlayerLevel& level) noexcept;

        MultiPlayerLevel* mPreviousLevel{};
    };

    class CaptureGuard {
    public:
        CaptureGuard(CaptureGuard const&)            = delete;
        CaptureGuard& operator=(CaptureGuard const&) = delete;
        CaptureGuard(CaptureGuard&&)                 = delete;
        CaptureGuard& operator=(CaptureGuard&&)      = delete;

        ~CaptureGuard() noexcept;

        [[nodiscard]] explicit operator bool() const noexcept { return mAcquired; }

        [[nodiscard]] std::chrono::steady_clock::duration waited() const noexcept { return mWaited; }

    private:
        friend class ChunkMutationBarrier;

        CaptureGuard(bool acquired, std::chrono::steady_clock::duration waited) noexcept;

        void release() noexcept;

        bool                                mAcquired{};
        std::chrono::steady_clock::duration mWaited{};
    };

    [[nodiscard]] static TickBoundaryGuard enterTickBoundary(MultiPlayerLevel& level) noexcept;

    [[nodiscard]] static CaptureGuard capture(std::chrono::milliseconds timeout = std::chrono::milliseconds{2000});

    static void setActiveLevel(MultiPlayerLevel* level);
};

[[nodiscard]] bool hookChunkMutationBarrier(bool enable);

} // namespace playback::functions
