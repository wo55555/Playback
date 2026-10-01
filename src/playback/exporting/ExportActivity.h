#pragma once

#include <atomic>
#include <cstdint>

namespace playback::exporting {

namespace detail {

inline std::atomic_bool      gExportActivityActive{false};
inline std::atomic_bool      gOfflineRenderActivityActive{false};
inline std::atomic<uint64_t> gOfflineRenderSceneState{4};

inline constexpr uint64_t OfflineRenderSceneReadyBit   = 1;
inline constexpr uint64_t OfflineRenderCpuReadyBit     = 2;
inline constexpr uint64_t OfflineRenderGenerationShift = 2;

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

// Entry eligibility cannot be granted retroactively when an in-flight submit returns.
[[nodiscard]] inline bool
finishOfflineRenderSceneSubmission(detail::OfflineRenderSceneSubmissionTicket entry, bool carriesScene) noexcept {
    if (!carriesScene || !entry.cpuReady) return false;
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
    return true;
}

[[nodiscard]] inline bool isOfflineRenderSceneSubmitted() noexcept {
    return (detail::gOfflineRenderSceneState.load(std::memory_order_acquire) & detail::OfflineRenderSceneReadyBit) != 0;
}

inline void setExportActivityActive(bool active) noexcept {
    detail::gExportActivityActive.store(active, std::memory_order_release);
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
