#pragma once

#include <atomic>

namespace playback::exporting {

namespace detail {

inline std::atomic_bool gExportActivityActive{false};
inline std::atomic_bool gOfflineRenderActivityActive{false};

} // namespace detail

inline void setExportActivityActive(bool active) noexcept {
    detail::gExportActivityActive.store(active, std::memory_order_release);
}

[[nodiscard]] inline bool isExportActivityActive() noexcept {
    return detail::gExportActivityActive.load(std::memory_order_acquire);
}

inline void setOfflineRenderActivityActive(bool active) noexcept {
    detail::gOfflineRenderActivityActive.store(active, std::memory_order_release);
}

[[nodiscard]] inline bool isOfflineRenderActivityActive() noexcept {
    return detail::gOfflineRenderActivityActive.load(std::memory_order_acquire);
}

namespace detail {

inline std::atomic_bool gOfflineRenderSceneSubmitted{false};

} // namespace detail

// Set by the BGFX submit hook once world geometry reached the GPU, consumed by the Present capture. The game
// thread returning from updateGraphics is not enough: BGFX submits on its own render thread.
inline void markOfflineRenderSceneSubmitted() noexcept {
    detail::gOfflineRenderSceneSubmitted.store(true, std::memory_order_release);
}

inline void clearOfflineRenderSceneSubmitted() noexcept {
    detail::gOfflineRenderSceneSubmitted.store(false, std::memory_order_release);
}

[[nodiscard]] inline bool consumeOfflineRenderSceneSubmitted() noexcept {
    return detail::gOfflineRenderSceneSubmitted.exchange(false, std::memory_order_acq_rel);
}

[[nodiscard]] inline bool isOfflineRenderSceneSubmitted() noexcept {
    return detail::gOfflineRenderSceneSubmitted.load(std::memory_order_acquire);
}

} // namespace playback::exporting
