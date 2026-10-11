#pragma once

#include "playback/exporting/ExportEcoMode.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace playback::exporting {

namespace detail {

inline std::atomic_bool      gExportActivityActive{false};
inline std::atomic_bool      gOfflineRenderActivityActive{false};
inline std::atomic<uint64_t> gOfflineRenderSceneState{8};

// Written only by the render thread; the generation is published last so readers see a matching timestamp.
inline std::atomic<int64_t>  gOfflineRenderSceneAcceptedAtNs{0};
inline std::atomic<uint64_t> gOfflineRenderSceneAcceptedGeneration{0};
inline std::atomic<uint64_t> gOfflineRenderOverlayGeneration{0};
inline std::atomic<uint32_t> gOfflineRenderOverlaySubmits{0};

inline constexpr uint64_t OfflineRenderSceneReadyBit   = 1;
inline constexpr uint64_t OfflineRenderCpuReadyBit     = 2;
inline constexpr uint64_t OfflineRenderEarlySceneBit   = 4;
inline constexpr uint64_t OfflineRenderGenerationShift = 3;

struct OfflineRenderSceneSubmissionTicket {
    uint64_t generation{};
    bool     cpuReady{};
};

} // namespace detail

inline uint64_t nextOfflineRenderSceneGeneration(uint64_t state) noexcept {
    auto generation =
        ((state >> detail::OfflineRenderGenerationShift) + 1) & (UINT64_MAX >> detail::OfflineRenderGenerationShift);
    if (generation == 0) generation = 1;
    return generation;
}

inline uint64_t invalidateOfflineRenderSceneSample() noexcept {
    auto state = detail::gOfflineRenderSceneState.load(std::memory_order_acquire);
    for (;;) {
        auto const generation = nextOfflineRenderSceneGeneration(state);
        auto const desired    = generation << detail::OfflineRenderGenerationShift;
        if (detail::gOfflineRenderSceneState
                .compare_exchange_weak(state, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return generation;
        }
    }
}

inline void clearOfflineRenderSceneSubmitted() noexcept { (void)invalidateOfflineRenderSceneSample(); }

inline bool invalidateOfflineRenderSceneSampleIfCurrent(uint64_t expectedGeneration) noexcept {
    auto state = detail::gOfflineRenderSceneState.load(std::memory_order_acquire);
    for (;;) {
        if ((state >> detail::OfflineRenderGenerationShift) != expectedGeneration) return false;
        auto const generation = nextOfflineRenderSceneGeneration(state);
        auto const desired    = generation << detail::OfflineRenderGenerationShift;
        if (detail::gOfflineRenderSceneState
                .compare_exchange_weak(state, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
}

[[nodiscard]] inline uint64_t beginOfflineRenderSceneSample() noexcept { return invalidateOfflineRenderSceneSample(); }

[[nodiscard]] inline uint64_t offlineRenderSceneGeneration() noexcept {
    return detail::gOfflineRenderSceneState.load(std::memory_order_acquire) >> detail::OfflineRenderGenerationShift;
}

[[nodiscard]] inline bool permitOfflineRenderSceneSample(uint64_t generation) noexcept {
    auto state = detail::gOfflineRenderSceneState.load(std::memory_order_acquire);
    do {
        if ((state >> detail::OfflineRenderGenerationShift) != generation) return false;
    } while (!detail::gOfflineRenderSceneState.compare_exchange_weak(
        state,
        state | detail::OfflineRenderCpuReadyBit,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    ));
    return true;
}

[[nodiscard]] inline detail::OfflineRenderSceneSubmissionTicket offlineRenderSceneSubmissionTicket() noexcept {
    auto const state = detail::gOfflineRenderSceneState.load(std::memory_order_acquire);
    return {
        state >> detail::OfflineRenderGenerationShift,
        (state & detail::OfflineRenderCpuReadyBit) != 0,
    };
}

// The render thread can start a sample's own scene submit before updateGraphics returns and grants the permit.
inline bool markOfflineRenderEarlyScene(uint64_t generation) noexcept {
    auto state = detail::gOfflineRenderSceneState.load(std::memory_order_acquire);
    do {
        if ((state >> detail::OfflineRenderGenerationShift) != generation) return false;
    } while (!detail::gOfflineRenderSceneState.compare_exchange_weak(
        state,
        state | detail::OfflineRenderEarlySceneBit,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    ));
    return true;
}

// Overlay-only submits that entered after the permit of the still-pending current generation.
[[nodiscard]] inline uint32_t offlineRenderOverlaySubmitsAfterPermit(uint64_t generation) noexcept {
    if (detail::gOfflineRenderOverlayGeneration.load(std::memory_order_acquire) != generation) return 0;
    return detail::gOfflineRenderOverlaySubmits.load(std::memory_order_relaxed);
}

inline void countOfflineRenderOverlaySubmit(detail::OfflineRenderSceneSubmissionTicket entry) noexcept {
    if (!entry.cpuReady) return;
    auto const state = detail::gOfflineRenderSceneState.load(std::memory_order_acquire);
    if ((state >> detail::OfflineRenderGenerationShift) != entry.generation
        || (state & detail::OfflineRenderSceneReadyBit) != 0) {
        return;
    }
    if (detail::gOfflineRenderOverlayGeneration.load(std::memory_order_relaxed) != entry.generation) {
        detail::gOfflineRenderOverlaySubmits.store(1, std::memory_order_relaxed);
        detail::gOfflineRenderOverlayGeneration.store(entry.generation, std::memory_order_release);
        return;
    }
    detail::gOfflineRenderOverlaySubmits.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline std::optional<std::chrono::steady_clock::time_point>
offlineRenderSceneAcceptedAt(uint64_t generation) noexcept {
    if (generation == 0
        || detail::gOfflineRenderSceneAcceptedGeneration.load(std::memory_order_acquire) != generation) {
        return std::nullopt;
    }
    return std::chrono::steady_clock::time_point{std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::nanoseconds{detail::gOfflineRenderSceneAcceptedAtNs.load(std::memory_order_relaxed)}
    )};
}

// Entry eligibility cannot be granted retroactively when an in-flight submit returns.
[[nodiscard]] inline bool
finishOfflineRenderSceneSubmission(detail::OfflineRenderSceneSubmissionTicket entry, bool carriesScene) noexcept {
    if (!carriesScene) {
        countOfflineRenderOverlaySubmit(entry);
        return false;
    }
    if (!entry.cpuReady) {
        (void)markOfflineRenderEarlyScene(entry.generation);
        return false;
    }
    auto state = detail::gOfflineRenderSceneState.load(std::memory_order_acquire);
    do {
        if ((state >> detail::OfflineRenderGenerationShift) != entry.generation
            || (state & detail::OfflineRenderCpuReadyBit) == 0) {
            return false;
        }
    } while (!detail::gOfflineRenderSceneState.compare_exchange_weak(
        state,
        (entry.generation << detail::OfflineRenderGenerationShift) | detail::OfflineRenderCpuReadyBit
            | detail::OfflineRenderSceneReadyBit,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    ));
    detail::gOfflineRenderSceneAcceptedAtNs.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count(),
        std::memory_order_relaxed
    );
    detail::gOfflineRenderSceneAcceptedGeneration.store(entry.generation, std::memory_order_release);
    return true;
}

[[nodiscard]] inline bool isOfflineRenderSceneSubmitted() noexcept {
    return (detail::gOfflineRenderSceneState.load(std::memory_order_acquire) & detail::OfflineRenderSceneReadyBit) != 0;
}

// True when a scene submit of this generation started before the permit and no later one has been accepted.
[[nodiscard]] inline bool wasOfflineRenderSceneMissed(uint64_t generation) noexcept {
    auto const state = detail::gOfflineRenderSceneState.load(std::memory_order_acquire);
    return (state >> detail::OfflineRenderGenerationShift) == generation
        && (state & detail::OfflineRenderEarlySceneBit) != 0 && (state & detail::OfflineRenderSceneReadyBit) == 0;
}

// True while this generation is still current and no scene submit has been accepted for it.
[[nodiscard]] inline bool isOfflineRenderScenePending(uint64_t generation) noexcept {
    auto const state = detail::gOfflineRenderSceneState.load(std::memory_order_acquire);
    return (state >> detail::OfflineRenderGenerationShift) == generation
        && (state & detail::OfflineRenderSceneReadyBit) == 0;
}

inline void setExportActivityActive(bool active) noexcept {
    if (detail::gExportActivityActive.exchange(active, std::memory_order_acq_rel) != active) {
        applyExportEcoMode(active);
    }
}

[[nodiscard]] inline bool isExportActivityActive() noexcept {
    return detail::gExportActivityActive.load(std::memory_order_acquire);
}

inline void setOfflineRenderActivityActive(bool active) noexcept {
    detail::gOfflineRenderActivityActive.store(active, std::memory_order_release);
    if (!active) clearOfflineRenderSceneSubmitted();
}

[[nodiscard]] inline bool isOfflineRenderActivityActive() noexcept {
    return detail::gOfflineRenderActivityActive.load(std::memory_order_acquire);
}

} // namespace playback::exporting
