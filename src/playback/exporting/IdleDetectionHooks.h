#pragma once

namespace playback::exporting {

[[nodiscard]] bool hookIdleDetection(bool enable);
[[nodiscard]] bool isIdleDetectionGuardInstalled() noexcept;

} // namespace playback::exporting
