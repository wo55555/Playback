#pragma once

#include <atomic>

namespace playback::editor::exporting {

namespace detail {

// The renderer and UI run on the client thread while the frame writer may run
// on a worker thread. Keep the export lifecycle visible to small client-side
// hooks without coupling those hooks to ReplayExportDriver's instance.
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

} // namespace playback::editor::exporting
