#pragma once

namespace playback::screen {

[[nodiscard]] bool hookIdleDetection(bool enable);
[[nodiscard]] bool isIdleDetectionGuardInstalled() noexcept;

} // namespace playback::screen
